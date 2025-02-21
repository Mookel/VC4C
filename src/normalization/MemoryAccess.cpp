/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "MemoryAccess.h"

#include "../Expression.h"
#include "../InstructionWalker.h"
#include "../Module.h"
#include "../Profiler.h"
#include "../intermediate/IntermediateInstruction.h"
#include "../intermediate/operators.h"
#include "../optimization/Optimizer.h"
#include "../periphery/VPM.h"
#include "AddressCalculation.h"
#include "MemoryMappings.h"
#include "log.h"

#include <algorithm>

using namespace vc4c;
using namespace vc4c::normalization;
using namespace vc4c::intermediate;
using namespace vc4c::periphery;
using namespace vc4c::operators;

// TODO make use of parameter's maxByteOffset? E.g. for caching?

InstructionWalker normalization::accessGlobalData(
    const Module& module, Method& method, InstructionWalker it, const Configuration& config)
{
    /*
     * Map pointer to global data to the start-of-global-data parameter
     *  plus the offset of the global data
     */
    for(std::size_t i = 0; i < it->getArguments().size(); ++i)
    {
        const auto& arg = it->assertArgument(i);
        if(arg.checkLocal() && arg.local()->is<Global>())
        {
            if(auto globalOffset = module.getGlobalDataOffset(arg.local()))
            {
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Replacing access to global data: " << it->to_string() << logging::endl);
                Value tmp = UNDEFINED_VALUE;
                if(globalOffset.value() == 0)
                {
                    tmp = method.findOrCreateBuiltin(BuiltinLocal::Type::GLOBAL_DATA_ADDRESS)->createReference();
                }
                else
                {
                    // emplace calculation of global-data pointer and replace argument
                    tmp = assign(it, TYPE_INT32, "%global_data_offset") =
                        method.findOrCreateBuiltin(BuiltinLocal::Type::GLOBAL_DATA_ADDRESS)->createReference() +
                        Value(Literal(globalOffset.value()), TYPE_INT32);
                }
                it->setArgument(i, std::move(tmp));
            }
        }
    }
    return it;
}

void normalization::spillLocals(const Module& module, Method& method, const Configuration& config)
{
    static constexpr std::size_t MINIMUM_THRESHOLD = 128; /* TODO some better limit */

    /*
     * 1. find all candidate locals for spilling:
     * - no labels (since they are never mapped to registers)
     * - only one write (for now, for easier handling)
     * - not used only locally within a minimum range, since those locals are more likely to be mapped to registers
     */
    // tracks the locals and their writing instructions
    FastMap<const Local*, InstructionWalker> spillingCandidates;
    /* TODO rewrite
    for(const auto& pair : method.readLocals())
    {
        if(pair.second.type == TYPE_LABEL)
            continue;
        // XXX for now, only select locals which are written just once
        // or maybe never (not yet), e.g. for hidden parameter
        // or written several times but read only once
        // TODO also include explicit parameters
        auto numWrites = pair.second.getUsers(LocalUse::Type::WRITER).size();
        auto numReads = pair.second.getUsers(LocalUse::Type::READER).size();
        if((numWrites <= 1 && numReads > 0) || (numWrites >= 1 && numReads == 1))
        {
            spillingCandidates.emplace(&pair.second, InstructionWalker{});
        }
    }
    */

    InstructionWalker it = method.walkAllInstructions();
    // skip all leading empty basic blocks
    while(!it.isEndOfMethod() && it.get<intermediate::BranchLabel>())
        it.nextInMethod();
    auto candIt = spillingCandidates.begin();
    while(!it.isEndOfMethod() && candIt != spillingCandidates.end())
    {
        // check if only read the first X instructions (from the start of the kernel), e.g. for (hidden) parameter,
        // where the next check fails,  since they are not yet written anywhere
        if(method.isLocallyLimited(it, candIt->first, MINIMUM_THRESHOLD))
            candIt = spillingCandidates.erase(candIt);
        else
            ++candIt;
    }
    while(!it.isEndOfMethod() && !spillingCandidates.empty())
    {
        // TODO if at some point all Basic block have references to their used locals, remove all locals which are used
        // just in one basic block instead of this logic??
        FastMap<const Local*, InstructionWalker>::iterator cIt;
        if(it->checkOutputLocal() &&
            (cIt = spillingCandidates.find(it->getOutput()->local())) != spillingCandidates.end())
        {
            if(method.isLocallyLimited(it, it->getOutput()->local(), MINIMUM_THRESHOLD))
                spillingCandidates.erase(cIt);
            else
                cIt->second = it;
        }
        it.nextInMethod();
    }

    LCOV_EXCL_START
    CPPLOG_LAZY_BLOCK(logging::Level::DEBUG, {
        for(const auto& pair : spillingCandidates)
        {
            logging::debug() << "Spilling candidate: " << pair.first->to_string() << " ("
                             << pair.first->countUsers(LocalUse::Type::WRITER) << " writes, "
                             << pair.first->countUsers(LocalUse::Type::READER) << " reads)" << logging::endl;
        }
    });
    LCOV_EXCL_STOP

    // TODO do not preemptively spill, only on register conflicts. Which case??
}

void normalization::resolveStackAllocation(
    const Module& module, Method& method, InstructionWalker it, const Configuration& config)
{
    // 1. calculate the offsets from the start of one QPU's "stack", heed alignment!
    // This is done in Normalizer

    const std::size_t stackBaseOffset = method.getStackBaseOffset();
    const std::size_t maximumStackSize = method.calculateStackSize();

    for(std::size_t i = 0; i < it->getArguments().size(); ++i)
    {
        const Value& arg = it->assertArgument(i);
        if(arg.checkLocal() && arg.type.getPointerType() && arg.local()->is<StackAllocation>())
        {
            auto stackAllocation = arg.local()->as<StackAllocation>();
            // 2.remove the life-time instructions
            if(it.get<intermediate::LifetimeBoundary>() != nullptr)
            {
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Dropping life-time instruction for stack-allocation: " << arg.to_string() << logging::endl);
                it = it.erase();
                // to not skip the next instruction
                it.previousInBlock();
            }
            else if(stackBaseOffset == 0 && maximumStackSize == 0 && stackAllocation->isLowered)
            {
                /*
                 * Stack objects which are lowered into VPM (or registers) have a special address calculation.
                 *
                 * Instead of the per-QPU stack frame and within that the per-stack object offset (see below),
                 * lowered stack objects have a global per-object offset within the VPM and a per-QPU offset within
                 * that.
                 *
                 * E.g. the VPM layout of lowered stack objects is as follows:
                 *
                 * | object A . QPU0 . QPU1 . QPU2 . ... | object B . QPU0 . QPU1 . QPU2 . ... | ...
                 *
                 * To convert the "normal" per-QPU/per-object layout (below) to the per-object/per-QPU layout, the
                 * per-QPU/per-object is subtracted from the pointer generated by the "normal" method (see
                 * #insertAddressToOffset) and then the per-QPU offset is added again.
                 *
                 * If all stack objects are lowered into VPM, there is no offset from the stack base that we need to
                 * subtract (the offset is always zero), so we can just set it to zero.
                 */
                it->setArgument(i, Value(INT_ZERO));
            }
            else
            {
                // 3. map the addresses to offsets from global-data pointer (see #accessGlobalData)
                /*
                 * Stack allocations are located in the binary data after the global data.
                 *
                 *
                 * To reduce the number of calculations, all stack allocations are grouped by their QPU, so the layout
                 * is as follows:
                 *
                 * | "Stack" of QPU0 | "Stack" of QPU1 | ...
                 *
                 * The offset of a single stack allocation can be calculated as:
                 * global-data address + global-data size + (QPU-ID * stack allocations maximum size) + offset of stack
                 * allocation = global-data address + (QPU-ID * stack allocations maximum size) + (global-data size +
                 * offset of stack allocation)
                 */
                // TODO to save instructions, could pre-calculate 'global-data address + global-data size + (QPU-ID *
                // stack allocations maximum size)' once, if any stack-allocation exists ??

                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Replacing access to stack allocated data: " << it->to_string() << logging::endl);

                auto qpuOffset = assign(it, TYPE_INT32, "%stack_offset") = mul24(Value(REG_QPU_NUMBER, TYPE_INT8),
                    Value(Literal(static_cast<uint32_t>(maximumStackSize)), TYPE_INT32));
                auto addrTemp = assign(it, arg.type, "%stack_addr") =
                    qpuOffset + method.findOrCreateBuiltin(BuiltinLocal::Type::GLOBAL_DATA_ADDRESS)->createReference();
                auto finalAddr = assign(it, arg.type, "%stack_addr") = addrTemp +
                    Value(Literal(static_cast<uint32_t>(stackAllocation->offset + stackBaseOffset)), TYPE_INT32);
                finalAddr.local()->set(ReferenceData(*arg.local(), ANY_ELEMENT));
                it->setArgument(i, std::move(finalAddr));
            }
        }
    }
}

static tools::SmallSortedPointerSet<const MemoryInfo*> getMemoryInfos(const Local* baseLocal,
    const FastMap<const Local*, MemoryInfo>& infos,
    const FastMap<const Local*, FastSet<const Local*>>& additionalAreaMappings)
{
    tools::SmallSortedPointerSet<const MemoryInfo*> result;
    // directly found, single area
    auto srcInfoIt = baseLocal ? infos.find(baseLocal) : infos.end();
    if(srcInfoIt != infos.end())
        result.emplace(&srcInfoIt->second);
    else if(baseLocal)
    {
        auto srcInfosIt = additionalAreaMappings.find(baseLocal);
        if(srcInfosIt != additionalAreaMappings.end())
        {
            for(auto conditionalSource : srcInfosIt->second)
            {
                srcInfoIt = infos.find(conditionalSource);
                if(srcInfoIt == infos.end())
                    throw CompilationError(CompilationStep::NORMALIZER,
                        "Memory info for conditionally addresses memory location not found",
                        conditionalSource->to_string());
                result.emplace(&srcInfoIt->second);
            }
        }
    }
    return result;
}

static bool checkIdDecoration(InstructionDecorations deco)
{
    return has_flag(deco, intermediate::InstructionDecorations::BUILTIN_LOCAL_ID) ||
        has_flag(deco, intermediate::InstructionDecorations::BUILTIN_GLOBAL_ID);
}

static bool hasOnlyAddressesDerivateOfLocalId(const MemoryAccessRange& range, unsigned& minFactor, unsigned& maxSize)
{
    // Be conservative, if there are no dynamic address parts in the container, don't assume that there are none, but
    // that we might have failed/skipped to determine them. Also if all work-items statically access the same index, we
    // do have a cross-item access.
    return !range.dynamicAddressParts.empty() &&
        std::all_of(range.dynamicAddressParts.begin(), range.dynamicAddressParts.end(),
            [&, memWrite{range.addressWrite.get<MemoryInstruction>()}](
                const std::pair<Value, intermediate::InstructionDecorations>& parts) -> bool {
                if(checkIdDecoration(parts.second))
                {
                    // The offset is in number of elements
                    minFactor = std::min(
                        minFactor, static_cast<unsigned>(range.memoryObject->type.getElementType().getVectorWidth()));
                    maxSize = std::max(
                        maxSize, static_cast<unsigned>(range.memoryObject->type.getElementType().getVectorWidth()));
                    return true;
                }
                if(!memWrite)
                    return false;
                std::shared_ptr<Expression> expression;
                if(auto writer = parts.first.getSingleWriter())
                    expression = Expression::createRecursiveExpression(*writer);
                if(expression && expression->hasConstantOperand() &&
                    (expression->code == Expression::FAKEOP_UMUL || expression->code == OP_MUL24 ||
                        expression->code == OP_SHL))
                {
                    // E.g. something like %global_id * X is allowed as long as X >= number of elements accessed per
                    // work-item. Also accept shl with constant, since this is also a multiplication.
                    bool leftIsId = expression->arg0.checkExpression() &&
                        checkIdDecoration(expression->arg0.checkExpression()->deco);
                    bool rightIsId = expression->arg1.checkExpression() &&
                        checkIdDecoration(expression->arg1.checkExpression()->deco);
                    auto constantArg =
                        leftIsId ? expression->arg1.getLiteralValue() : expression->arg0.getLiteralValue();
                    if(constantArg && leftIsId != rightIsId)
                    {
                        // we have a multiplication (maybe presenting as a shift) of the global/local ID with a
                        // constant, now we need to make sure the constant is at least the number of elements accessed.

                        auto factor = expression->code == OP_SHL ? (1u << constantArg->unsignedInt()) :
                                                                   constantArg->unsignedInt();
                        if(memWrite->op == MemoryOperation::READ)
                        {
                            minFactor = std::min(minFactor, factor);
                            maxSize = std::max(
                                maxSize, static_cast<unsigned>(memWrite->getDestinationElementType().getVectorWidth()));
                            return true;
                        }
                        if(memWrite->op == MemoryOperation::WRITE)
                        {
                            minFactor = std::min(minFactor, factor);
                            maxSize = std::max(
                                maxSize, static_cast<unsigned>(memWrite->getSourceElementType().getVectorWidth()));
                            return true;
                        }
                    }
                }
                return false;
            });
}

static bool mayHaveCrossWorkItemMemoryDependency(const Local* memoryObject, const MemoryInfo& info)
{
    if(check(memoryObject->as<Global>()) & &Global::isConstant ||
            check(memoryObject->as<Parameter>()) & [](const Parameter& param) -> bool {
           return has_flag(param.decorations, ParameterDecorations::READ_ONLY);
       })
        // constant memory -> no write -> no dependency
        return false;
    switch(info.type)
    {
    case MemoryAccessType::RAM_LOAD_TMU:
        // load of constant data -> no data dependency possible
        return false;
    case MemoryAccessType::QPU_REGISTER_READONLY:
    case MemoryAccessType::QPU_REGISTER_READWRITE:
    case MemoryAccessType::VPM_PER_QPU:
        // data not shared -> no data dependency possible
        return false;
    default:
        // memory access type allows for read/write -> need further access range checking
        break;
    }

    if(info.ranges)
    {
        unsigned minFactor = std::numeric_limits<unsigned>::max();
        unsigned maxSize = 0;
        if(std::all_of(info.ranges->begin(), info.ranges->end(),
               [&](const MemoryAccessRange& range) -> bool {
                   return hasOnlyAddressesDerivateOfLocalId(range, minFactor, maxSize);
               }) &&
            maxSize <= minFactor)
            // If we manged to figure out the dynamic address parts to be (a derivation of) the local or global id, and
            // the maximum accessed vector size is not larger than the minimum accessed local/global id factor, then we
            // don't have data dependencies across different local ids.
            return false;
    }

    CPPLOG_LAZY(logging::Level::DEBUG,
        log << "Memory access might have cross work-item data dependency: " << memoryObject->to_string() << " ("
            << info.to_string() << ')' << logging::endl);
    return true;
}

/* clang-format off */
/*
 * Matrix of memory types and storage locations:
 *
 *           | global | local | private | constant
 * buffer    |   -    |VPM/GD | QPU/VPM | QPU/GD
 * parameter |  RAM   |RAM/(*)|    -    |   RAM
 *
 * buffer is both inside and outside of function scope (where allowed)
 * - : is not allowed by OpenCL
 * (*) is lowered into VPM if the highest index accessed is known and fits
 * GD: global data segment of kernel buffer
 * RAM: load via TMU if possible (not written to), otherwise use VPM
 *
 * Sources:
 * https://stackoverflow.com/questions/22471466/why-program-global-scope-variables-must-be-constant#22474119
 * https://stackoverflow.com/questions/17431941/how-to-use-arrays-in-program-global-scope-in-opencl
 *
 * 
 * Matrix of memory types and access ways:
 * compile-time memory: __constant buffer with values known at compile-time
 * constant memory: __constant or read-only __global/__local buffer/parameter
 * private memory: __private buffer/stack allocations
 * read-write memory: any other __global/__local buffer/parameter
 *
 *                     |   optimization   |   location   |   read    |   write   |    copy from    |       copy to       | group | priority |
 * compile-time memory |     "normal"     |      GD      |    TMU    |     -     |    DMA/TMU(*)   |          -          |  (1)  |     2    |    
 *                     |   lowered load   |      QPU     | register  |     -     | VPM/register(*) |          -          |  (2)  |     1    |
 * constant memory     |     "normal"     |     GD/RAM   |    TMU    |     -     |    DMA/TMU(*)   |          -          |  (1)  |     2    |
 * private memory      |     "normal"     |      GD      |    DMA    |    DMA    |       DMA       |         DMA         |  (3)  |     3    |
 *                     | lowered register |      QPU     | register  | register  | VPM/register(*) | VPM/TMU/register(*) |  (2)  |     1    |
 *                     |   lowered VPM    |      VPM     |    VPM    |    VPM    |     VPM/DMA     |       VPM/DMA       |  (4)  |     2    |
 * read-write memory   |     "normal"     |     GD/RAM   |    DMA    |    DMA    |       DMA       |         DMA         |  (3)  |     3    |
 *                     |   lowered VPM    |      VPM     |    VPM    |    VPM    |     VPM/DMA     |       VPM/DMA       |  (4)  |     1    |
 *                     |    cached VPM    | VPM + GD/RAM | VPM + DMA | VPM + DMA |     VPM/DMA     |       VPM/DMA       |  (4)  |     2    |
 *
 * Special cases:
 *  (*) when copying from constant memory into register, TMU can be used instead. Copying from and to register is done inside the QPU
 *
 */
/* clang-format on */

void normalization::mapMemoryAccess(const Module& module, Method& method, const Configuration& config)
{
    /*
     * 1. lower constant/private buffers into register
     *    lower global constant buffers into registers
     *    lower small enough private buffers to registers
     * 2. generate TMU loads for read-only memory
     *    keep all read-only parameters in RAM, load via TMU
     *    also load constants via TMU, which could not be lowered into register
     * 3. lower per-QPU (private) buffers into VPM
     * 4. lower shared buffers (local) into VPM
     * 5. generate remaining instructions for RAM access via VPM scratch area
     * TODO:
     * 3.1 for memory located in RAM, try to group/queue reads/writes
     * 3.2 also try to use VPM as cache (e.g. only write back into memory when VPM cache area full, prefetch into VPM)
     * 4. final pass which actually converts VPM cache
     */

    // determine preferred and fall-back memory access type for each memory are
    auto memoryAccessInfo = determineMemoryAccess(method);

    FastMap<const Local*, MemoryInfo> infos;
    FastMap<const Local*, CacheMemoryData> localsCachedInVPM;
    bool allowVPMCaching = optimizations::Optimizer::isEnabled(optimizations::PASS_CACHE_MEMORY, config);
    {
        // gather more information about the memory areas and modify the access types. E.g. if the preferred access type
        // cannot be used, use the fall-back
        infos.reserve(memoryAccessInfo.memoryAccesses.size());
        for(auto& mapping : memoryAccessInfo.memoryAccesses)
        {
            auto it = infos.emplace(mapping.first, checkMemoryMapping(method, mapping.first, mapping.second)).first;
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << (it->first->is<Parameter>() ? "Parameter" :
                                                     (it->first->is<StackAllocation>() ? "Stack variable" : "Local"))
                    << " '" << it->first->to_string() << "' will be mapped to: " << it->second.to_string()
                    << logging::endl);
            if(allowVPMCaching && it->second.type == MemoryAccessType::RAM_READ_WRITE_VPM && it->second.area)
            {
                // access memory in RAM, but cache in VPM ->store for pre-load and write-back and treat as lowered to
                // VPM
                localsCachedInVPM.emplace(it->first, CacheMemoryData{&it->second, false, false});
                it->second.type = MemoryAccessType::VPM_SHARED_ACCESS;
            }
            // TODO if we disallow the caching, the VPM cache rows are still allocated!
        }
    }

    if(std::none_of(infos.begin(), infos.end(), [](const std::pair<const Local*, MemoryInfo>& info) -> bool {
           return mayHaveCrossWorkItemMemoryDependency(info.first, info.second);
       }))
        // We can reason that no work-item (across work-group loops) accesses memory written by another work-item
        // (except maybe the work-item of the previous loop with the same local ID) and thus we can omit the work-group
        // synchronization barrier blocks, since there is no possible data races we need to guard against.
        method.flags = add_flag(method.flags, MethodFlags::NO_CROSS_ITEM_MEMORY_ACCESS);

    // list of basic blocks where multiple VPM accesses could be combined
    FastSet<BasicBlock*> affectedBlocks;

    // TODO sort locals by where to put them and then call 1. check of mapping and 2. mapping on all
    for(auto& memIt : memoryAccessInfo.accessInstructions)
    {
        auto mem = memIt.get<const MemoryInstruction>();
        auto srcBaseLocal = mem->getSource().checkLocal() ? mem->getSource().local()->getBase(true) : nullptr;
        auto dstBaseLocal = mem->getDestination().checkLocal() ? mem->getDestination().local()->getBase(true) : nullptr;

        auto sourceInfos = getMemoryInfos(srcBaseLocal, infos, memoryAccessInfo.additionalAreaMappings);
        auto destInfos = getMemoryInfos(dstBaseLocal, infos, memoryAccessInfo.additionalAreaMappings);

        auto checkVPMAccess = [](const MemoryInfo* info) { return info->type == MemoryAccessType::RAM_READ_WRITE_VPM; };
        if(std::any_of(sourceInfos.begin(), sourceInfos.end(), checkVPMAccess) ||
            std::any_of(destInfos.begin(), destInfos.end(), checkVPMAccess))
            affectedBlocks.emplace(InstructionWalker{memIt}.getBasicBlock());

        mapMemoryAccess(method, memIt, const_cast<MemoryInstruction*>(mem), sourceInfos, destInfos);

        // enrich caching information with input/output locals
        for(auto* info : sourceInfos)
        {
            auto cacheIt = localsCachedInVPM.find(info->local);
            if(cacheIt != localsCachedInVPM.end())
                // we read, so pre-load
                // XXX could be omitted if we can guarantee every entry to be written before read (e.g. everything
                // written before barrier() and only read afterwards)
                cacheIt->second.insertPreload = true;
        }
        for(auto* info : destInfos)
        {
            auto cacheIt = localsCachedInVPM.find(info->local);
            if(cacheIt != localsCachedInVPM.end())
            {
                // we write, so write-back
                cacheIt->second.insertWriteBack = true;
                // TODO unless we can prove to overwrite all of the data in any case, we need to initially fill the
                // cache with the original data to not write garbage values back to the RAM
                cacheIt->second.insertPreload = true;
            }
        }
    }

    method.vpm->dumpUsage();

    insertCacheSynchronizationCode(method, localsCachedInVPM);

    // TODO clean up no longer used (all kernels!) globals and stack allocations

    // clean up empty instructions
    method.cleanEmptyInstructions();
    PROFILE_COUNTER(
        vc4c::profiler::COUNTER_GENERAL + 80, "Scratch memory size (in rows)", method.vpm->getScratchArea().numRows);
}
