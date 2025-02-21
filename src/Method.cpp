/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "Method.h"

#include "Module.h"
#include "Profiler.h"
#include "analysis/ControlFlowGraph.h"
#include "intermediate/IntermediateInstruction.h"
#include "periphery/VPM.h"

#include "log.h"

#include <atomic>

using namespace vc4c;

// TODO track locals via thread-not-safe shared_ptr. Method itself tracks as weak_ptr,
// so local is erased when there is no more use. Local#reference also is shared_ptr

Method::Method(Module& module) :
    flags(MethodFlags::NONE), name(), returnType(TYPE_UNKNOWN),
    vpm(new periphery::VPM(module.compilationConfig.availableVPMSize)), module(module)
{
}

Method::~Method()
{
    // makes sure, instructions are removed before locals (so usages are all zero)
    basicBlocks.clear();
}

const BuiltinLocal* Method::findBuiltin(BuiltinLocal::Type type) const
{
    if(builtinLocals.size() <= static_cast<std::size_t>(type))
        return nullptr;
    auto& entry = builtinLocals[static_cast<std::size_t>(type)];
    return entry.get();
}

const Parameter* Method::findParameter(const std::string& name) const
{
    for(const Parameter& param : parameters)
    {
        if(param.name == name)
            return &param;
    }
    return nullptr;
}

const Global* Method::findGlobal(const std::string& name) const
{
    return module.findGlobal(name);
}

const StackAllocation* Method::findStackAllocation(const std::string& name) const
{
    for(const StackAllocation& s : stackAllocations)
    {
        if(s.name == name)
            return &s;
    }
    return nullptr;
}

const Local* Method::createLocal(DataType type, const std::string& name)
{
    Local loc(type, name);
    addLocalData(loc);
    auto it = locals.emplace(std::move(loc)).first;
    return &(*it);
}

Parameter& Method::addParameter(Parameter&& param)
{
    parameters.emplace_back(std::move(param));
    auto& p = parameters.back();
    // Parameters of non-kernel functions might also be e.g. vectors of 64-bit integers in which case we add the lower
    // and upper parts there too
    addLocalData(p);
    return p;
}

const BuiltinLocal* Method::findOrCreateBuiltin(BuiltinLocal::Type type)
{
    using Type = BuiltinLocal::Type;
    if(builtinLocals.size() < BuiltinLocal::NUM_LOCALS)
        builtinLocals.resize(BuiltinLocal::NUM_LOCALS);
    auto& entry = builtinLocals.at(static_cast<std::size_t>(type));
    if(entry)
        return entry.get();
    switch(type)
    {
    case Type::WORK_DIMENSIONS:
        entry = std::make_unique<BuiltinLocal>("%work_dim", TYPE_INT32, type);
        return entry.get();
    case Type::LOCAL_SIZES:
        entry = std::make_unique<BuiltinLocal>("%local_sizes", TYPE_INT32, type);
        return entry.get();
    case Type::LOCAL_IDS:
        entry = std::make_unique<BuiltinLocal>("%local_ids", TYPE_INT32, type);
        return entry.get();
    case Type::NUM_GROUPS_X:
        entry = std::make_unique<BuiltinLocal>("%num_groups_x", TYPE_INT32, type);
        return entry.get();
    case Type::NUM_GROUPS_Y:
        entry = std::make_unique<BuiltinLocal>("%num_groups_y", TYPE_INT32, type);
        return entry.get();
    case Type::NUM_GROUPS_Z:
        entry = std::make_unique<BuiltinLocal>("%num_groups_z", TYPE_INT32, type);
        return entry.get();
    case Type::GROUP_ID_X:
        entry = std::make_unique<BuiltinLocal>("%group_id_x", TYPE_INT32, type);
        return entry.get();
    case Type::GROUP_ID_Y:
        entry = std::make_unique<BuiltinLocal>("%group_id_y", TYPE_INT32, type);
        return entry.get();
    case Type::GROUP_ID_Z:
        entry = std::make_unique<BuiltinLocal>("%group_id_z", TYPE_INT32, type);
        return entry.get();
    case Type::GROUP_IDS:
        entry = std::make_unique<BuiltinLocal>("%group_ids", TYPE_INT32.toVectorType(3), type);
        return entry.get();
    case Type::GLOBAL_OFFSET_X:
        entry = std::make_unique<BuiltinLocal>("%global_offset_x", TYPE_INT32, type);
        return entry.get();
    case Type::GLOBAL_OFFSET_Y:
        entry = std::make_unique<BuiltinLocal>("%global_offset_y", TYPE_INT32, type);
        return entry.get();
    case Type::GLOBAL_OFFSET_Z:
        entry = std::make_unique<BuiltinLocal>("%global_offset_z", TYPE_INT32, type);
        return entry.get();
    case Type::GLOBAL_DATA_ADDRESS:
        entry = std::make_unique<BuiltinLocal>("%global_data_address", TYPE_INT32, type);
        return entry.get();
    case Type::UNIFORM_ADDRESS:
        entry = std::make_unique<BuiltinLocal>("%uniform_address", TYPE_INT32, type);
        return entry.get();
    case Type::MAX_GROUP_ID_X:
        entry = std::make_unique<BuiltinLocal>("%max_group_id_x", TYPE_INT32, type);
        return entry.get();
    case Type::MAX_GROUP_ID_Y:
        entry = std::make_unique<BuiltinLocal>("%max_group_id_y", TYPE_INT32, type);
        return entry.get();
    case Type::MAX_GROUP_ID_Z:
        entry = std::make_unique<BuiltinLocal>("%max_group_id_z", TYPE_INT32, type);
        return entry.get();
    default:;
    }
    throw CompilationError(
        CompilationStep::GENERAL, "Unhandled built-in type", std::to_string(static_cast<unsigned>(type)));
}

static std::size_t removeUser(
    tools::SmallSortedPointerMap<const LocalUser*, LocalUse>& remainingUsers, const LocalUser* user)
{
    auto numUsers = remainingUsers.erase(user);
    if(auto comb = dynamic_cast<const intermediate::CombinedOperation*>(user))
    {
        if(auto op1 = comb->getFirstOp())
            numUsers += remainingUsers.erase(op1);
        if(auto op2 = comb->getSecondOp())
            numUsers += remainingUsers.erase(op2);
    }
    return numUsers;
}

static NODISCARD bool removeUsagesInBasicBlock(const Method& method, const BasicBlock& bb, const Local* locale,
    tools::SmallSortedPointerMap<const LocalUser*, LocalUse>& remainingUsers, int& usageRangeLeft)
{
    auto it = bb.walk();
    while(usageRangeLeft >= 0 && !it.isEndOfMethod())
    {
        removeUser(remainingUsers, it.get());
        --usageRangeLeft;
        if(auto branch = dynamic_cast<const intermediate::Branch*>(it.get()))
        {
            for(auto target : branch->getTargetLabels())
            {
                const BasicBlock* successor = method.findBasicBlock(target);
                if(successor && removeUsagesInBasicBlock(method, *successor, locale, remainingUsers, usageRangeLeft))
                    return true;
            }
        }
        it.nextInMethod();
    }
    return remainingUsers.empty();
}

bool Method::isLocallyLimited(InstructionWalker curIt, const Local* locale, const std::size_t threshold) const
{
    auto remainingUsers = locale->getUsers();

    int32_t usageRangeLeft = static_cast<int32_t>(threshold);
    // check whether the local is written in the instruction before (and this)
    // this happens e.g. for comparisons
    if(!curIt.isStartOfBlock())
    {
        removeUser(remainingUsers, curIt.copy().previousInBlock().get());
    }
    while(usageRangeLeft >= 0 && !curIt.isEndOfMethod())
    {
        removeUser(remainingUsers, curIt.get());
        --usageRangeLeft;
        if(auto branch = curIt.get<intermediate::Branch>())
        {
            for(auto target : branch->getTargetLabels())
            {
                const BasicBlock* successor = findBasicBlock(target);
                if(successor && removeUsagesInBasicBlock(*this, *successor, locale, remainingUsers, usageRangeLeft))
                    return true;
            }
            if(branch->isUnconditional())
                // this branch jumps away unconditionally and the successors do not have all remaining usages within
                // the remaining range, so we abort
                return false;
        }
        curIt.nextInMethod();
    }

    return remainingUsers.empty();
}

static std::atomic_size_t tmpIndex{0};

Value Method::addNewLocal(DataType type, const std::string& prefix, const std::string& postfix)
{
    const std::string name = createLocalName(prefix, postfix);
    return createLocal(type, name)->createReference();
}

std::string Method::createLocalName(const std::string& prefix, const std::string& postfix)
{
    // prefix, postfix empty -> "%tmp.tmpIndex"
    // prefix empty -> "%postfix"
    // postfix empty -> "prefix.tmpIndex"
    // none empty -> "prefix.postfix"
    std::string localName;
    if((prefix.empty() || prefix == "%") && postfix.empty())
    {
        localName = std::string("%tmp.") + std::to_string(tmpIndex++);
    }
    else if((prefix.empty() || prefix == "%"))
    {
        if(postfix[0] == '%')
            // to prevent "%%xyz"
            localName = postfix;
        else
            localName = std::string("%") + postfix;
    }
    else if(postfix.empty())
    {
        localName = (prefix + ".") + std::to_string(tmpIndex++);
    }
    else
    {
        localName = (prefix + ".") + postfix;
    }
    return localName;
}

InstructionWalker Method::walkAllInstructions()
{
    if(basicBlocks.empty())
        return InstructionWalker{};
    return begin()->walk();
}

void Method::forAllInstructions(const std::function<void(const intermediate::IntermediateInstruction&)>& consumer) const
{
    for(const BasicBlock& bb : *this)
    {
        for(const auto& instr : bb.instructions)
        {
            if(instr)
                consumer(*instr);
        }
    }
}

std::size_t Method::countInstructions() const
{
    std::size_t count = 0;
    for(const BasicBlock& bb : *this)
    {
        count += bb.instructions.size();
    }
    return count;
}

std::size_t Method::cleanEmptyInstructions()
{
    // TODO required??
    std::size_t num = 0;
    auto it = walkAllInstructions();
    while(!it.isEndOfMethod())
    {
        if(it.get() == nullptr)
        {
            it.erase();
            ++num;
            if(it.isEndOfBlock())
                it.nextInMethod();
        }
        else
            it.nextInMethod();
    }
    return num;
}

void Method::appendToEnd(intermediate::IntermediateInstruction* instr)
{
    if(auto label = dynamic_cast<intermediate::BranchLabel*>(instr))
    {
        basicBlocks.emplace_back(*this, label);
        updateCFGOnBlockInsertion(&basicBlocks.back());
    }
    else
    {
        checkAndCreateDefaultBasicBlock();
        basicBlocks.back().instructions.emplace_back(instr);
        if(cfg && dynamic_cast<intermediate::Branch*>(instr))
            updateCFGOnBranchInsertion(basicBlocks.back().walkEnd().previousInBlock());
    }
}
InstructionWalker Method::appendToEnd()
{
    checkAndCreateDefaultBasicBlock();
    // Invalidation of the CFG in this case is handled in InstructionWalker
    return basicBlocks.back().walkEnd();
}

std::size_t Method::getNumLocals() const
{
    return locals.size();
}

void Method::cleanLocals()
{
    // FIXME deletes locals which still have Local#reference to them
    // If locals are tracked via shared_ptr (weak_ptr in Method), walk through pointers and only remove when no more
    // shared references
    /*
    PROFILE_COUNTER(vc4c::profiler::COUNTER_GENERAL + 7, "Clean locals (before)", locals.size());
#ifdef DEBUG_MODE
    // check duplicate locals
    FastSet<std::string> localNames;
    for(const Global& g : module.globalData)
    {
        if(!localNames.emplace(g.name).second)
            throw CompilationError(CompilationStep::GENERAL, "Duplicate global", g.to_string());
    }
    for(const Parameter& p : parameters)
    {
        if(!localNames.emplace(p.name).second)
            throw CompilationError(CompilationStep::GENERAL, "Duplicate parameter for method", p.to_string());
    }
#endif
    auto it = locals.begin();
    std::size_t numCleaned = 0;
    while(it != locals.end())
    {
#ifdef DEBUG_MODE
        if(!localNames.emplace(it->first).second)
            throw CompilationError(
                CompilationStep::GENERAL, "Local is already defined for method", it->second.to_string());
#endif
        if((*it).second.getUsers().empty())
        {
            it = locals.erase(it);
            ++numCleaned;
        }
        else
            ++it;
    }
    logging::debug() << "Cleaned " << numCleaned << " unused locals from method " << name << logging::endl;
    PROFILE_COUNTER_WITH_PREV(vc4c::profiler::COUNTER_GENERAL + 8, "Clean locals (after)", locals.size(),
        vc4c::profiler::COUNTER_GENERAL + 7);
        */
}

LCOV_EXCL_START
void Method::dumpInstructions() const
{
    for(const BasicBlock& bb : *this)
    {
        bb.dumpInstructions();
    }
}
LCOV_EXCL_STOP

BasicBlock* Method::findBasicBlock(const Local* label)
{
    for(BasicBlock& bb : *this)
    {
        auto branchLabel = dynamic_cast<const intermediate::BranchLabel*>(bb.begin()->get());
        if(branchLabel && branchLabel->getLabel() == label)
            return &bb;
    }
    return nullptr;
}

const BasicBlock* Method::findBasicBlock(const Local* label) const
{
    for(const BasicBlock& bb : *this)
    {
        auto branchLabel = dynamic_cast<const intermediate::BranchLabel*>(bb.begin()->get());
        if(branchLabel && branchLabel->getLabel() == label)
            return &bb;
    }
    return nullptr;
}

BasicBlock* Method::findBasicBlock(const std::string& label)
{
    for(BasicBlock& bb : *this)
    {
        auto branchLabel = dynamic_cast<const intermediate::BranchLabel*>(bb.begin()->get());
        if(branchLabel && branchLabel->getLabel()->name == label)
            return &bb;
    }
    return nullptr;
}

const BasicBlock* Method::findBasicBlock(const std::string& label) const
{
    for(const BasicBlock& bb : *this)
    {
        auto branchLabel = dynamic_cast<const intermediate::BranchLabel*>(bb.begin()->get());
        if(branchLabel && branchLabel->getLabel()->name == label)
            return &bb;
    }
    return nullptr;
}

bool Method::removeBlock(BasicBlock& block, bool overwriteUsages)
{
    if(!overwriteUsages)
    {
        // check any usage
        // 1. check instructions inside block
        if(!block.empty())
            return false;
        // 2. check explicit jumps to this block
        unsigned count = 0;
        block.forPredecessors([&block, &count](InstructionWalker it) -> void {
            // only check for explicit jumps to this block, implicit "jumps" will just fall-through to the next block
            if(auto branch = it.get<intermediate::Branch>())
            {
                auto targets = branch->getTargetLabels();
                if(targets.find(block.getLabel()->getLabel()) != targets.end())
                    ++count;
            }
        });
        if(count > 0)
            return false;
    }
    auto it = begin();
    while(it != end())
    {
        if(&(*it) == &block)
        {
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Removing basic block '" << block.to_string() << "' from function " << name << logging::endl);
            updateCFGOnBlockRemoval(&(*it));
            basicBlocks.erase(it);
            return true;
        }
        ++it;
    }
    logging::warn() << "Basic block '" << block.to_string() << "' was not found in this function " << name
                    << logging::endl;
    return false;
}

BasicBlock& Method::createAndInsertNewBlock(BasicBlockList::iterator position, const std::string& labelName)
{
    auto newLabel = locals.emplace(Local(TYPE_LABEL, labelName));
    auto& block = *basicBlocks.emplace(position, *this, new intermediate::BranchLabel(*newLabel.first));
    updateCFGOnBlockInsertion(&block);
    return block;
}

InstructionWalker Method::emplaceLabel(InstructionWalker it, intermediate::BranchLabel* label)
{
    if(basicBlocks.empty())
    {
        auto& newBlock = *basicBlocks.emplace(basicBlocks.begin(), *this, label);
        updateCFGOnBlockInsertion(&newBlock);
        return newBlock.walk();
    }
    auto blockIt = begin();
    while(blockIt != end())
    {
        if(&(*blockIt) == it.basicBlock)
            break;
        ++blockIt;
    }
    if(blockIt == end())
        throw CompilationError(CompilationStep::GENERAL, "Failed to find basic block for instruction iterator",
            (it.has() ? it->to_string() : ""));
    // 1. insert new basic block after the current (or in front of it, if we emplace at the start of the basic block)
    bool isStartOfBlock = blockIt->walk() == it;
    if(!isStartOfBlock)
        ++blockIt;
    BasicBlock& newBlock = *basicBlocks.emplace(blockIt, *this, label);
    updateCFGOnBlockInsertion(&newBlock);
    // 2. move all instructions beginning with it (inclusive) to the new basic block
    while(!isStartOfBlock && !it.isEndOfBlock())
    {
        // using InstructionWalker here triggers updates of the CFG on moving branches
        newBlock.walkEnd().emplace(it.release());
        it.erase();
    }
    // 3. return the begin() of the new basic block
    return newBlock.walk();
}

void Method::calculateStackOffsets()
{
    // TODO this could be greatly improved, by re-using space for other stack-allocations, when their life-times don't
    // intersect (similar to register allocation)
    const std::size_t stackBaseOffset = getStackBaseOffset();

    // Simple version: reserve extra space for every stack-allocation
    std::size_t currentOffset = 0;
    for(auto& stackAllocation : stackAllocations)
    {
        if(stackAllocation.isLowered)
            // is lowered into VPM or register, does not participate in in-memory-stack
            continue;
        if((stackBaseOffset + currentOffset) % stackAllocation.alignment != 0)
            currentOffset +=
                stackAllocation.alignment - ((stackBaseOffset + currentOffset) % stackAllocation.alignment);
        stackAllocation.offset = currentOffset;
        currentOffset += stackAllocation.size;
    }

    /*
     * If a stack allocation is lowered to VPM or a register, we don't care about its "memory address". But, e.g. for
     * handling conditionally addressed registers (see #insertAddressToElementOffset), we do have to make sure that
     * its "memory address" is unique.
     *
     * Since lowered stack allocations have (so far) a calculated offset of zero, they all get assigned the same "memory
     * address" in #resolveStackAllocation. Thus, we make up some arbitrary address that
     * a) does not conflict with any actual address and
     * b) guarantees no other "memory range" for lowered stack allocations overlap with this one (e.g. we "reserve"
     * enough memory for the stack allocation to actually fit in)
     */
    for(auto& stackAllocation : stackAllocations)
    {
        if(!stackAllocation.isLowered)
            // already handled above
            continue;
        if((stackBaseOffset + currentOffset) % stackAllocation.alignment != 0)
            currentOffset +=
                stackAllocation.alignment - ((stackBaseOffset + currentOffset) % stackAllocation.alignment);
        stackAllocation.offset = currentOffset;
        currentOffset += stackAllocation.size;
    }
}

std::size_t Method::calculateStackSize() const
{
    if(stackAllocations.empty())
        return 0;
    const StackAllocation* max = nullptr;
    for(const StackAllocation& s : stackAllocations)
    {
        if(s.isLowered)
            // is lowered into VPM or register, does not participate in actual in-memory-stack
            continue;
        if(!max || (s.offset + s.size > max->offset + max->size))
            max = &s;
    }
    if(!max || max->isLowered)
        // stack allocation with highest offset is lowered to VPM or register
        // -> all stack allocations are lowered to VPM or register
        return 0;

    std::size_t stackSize = max->offset + max->size;

    // make sure, stack-size is aligned to maximum stack entry alignment (for 2nd, 3rd, ... stack-frame)
    std::size_t maxAlignment = 1;
    if(!stackAllocations.empty())
        maxAlignment = stackAllocations.begin()->alignment;
    if(stackSize % maxAlignment != 0)
        stackSize += maxAlignment - (stackSize % maxAlignment);
    // align size of stack-frame to at least 8 bytes, so the code-block is aligned correctly
    if(stackSize % 8 != 0)
        stackSize += 8 - (stackSize % 8);
    return stackSize;
}

std::size_t Method::getStackBaseOffset() const
{
    std::size_t baseOffset = module.getGlobalDataOffset(nullptr).value();

    std::size_t maxAlignment = 1;
    if(!stackAllocations.empty())
        maxAlignment = stackAllocations.begin()->alignment;

    if((baseOffset % maxAlignment) != 0)
        baseOffset += maxAlignment - (baseOffset % maxAlignment);

    // align offset of stack-frame to at least 8 bytes
    if(baseOffset % 8 != 0)
        baseOffset += 8 - (baseOffset % 8);
    return baseOffset;
}

analysis::ControlFlowGraph& Method::getCFG()
{
    if(!cfg)
    {
        CPPLOG_LAZY(
            logging::Level::DEBUG, log << "Creating/updating CFG for function: " << name << "..." << logging::endl);
        cfg = analysis::ControlFlowGraph::createCFG(*this);
    }
    return *cfg;
}

void Method::moveBlock(BasicBlockList::iterator origin, BasicBlockList::iterator dest)
{
    // splice removes the element pointed to by origin from the list (second) parameter and inserts it into the list
    // object at position dest without creating or destroying an object
    basicBlocks.splice(dest, basicBlocks, origin);
}

DataType Method::createPointerType(DataType elementType, AddressSpace addressSpace, unsigned alignment)
{
    return DataType(module.createPointerType(elementType, addressSpace, alignment));
}

DataType Method::createStructType(const std::string& name, const std::vector<DataType>& elementTypes, bool isPacked)
{
    return DataType(module.createStructType(name, elementTypes, isPacked));
}

DataType Method::createArrayType(DataType elementType, unsigned int size)
{
    return DataType(module.createArrayType(elementType, size));
}

DataType Method::createImageType(uint8_t dimensions, bool isImageArray, bool isImageBuffer, bool isSampled)
{
    return DataType(module.createImageType(dimensions, isImageArray, isImageBuffer, isSampled));
}

BasicBlock* Method::getNextBlockAfter(const BasicBlock* block)
{
    bool returnNext = false;
    for(BasicBlock& bb : *this)
    {
        if(returnNext)
            return &bb;
        if(&bb == block)
            returnNext = true;
    }
    return nullptr;
}

BasicBlock* Method::getPreviousBlock(const BasicBlock* block)
{
    bool returnNext = false;
    auto it = basicBlocks.rbegin();
    while(it != basicBlocks.rend())
    {
        if(returnNext)
            return &(*it);
        if(&(*it) == block)
            returnNext = true;
        --it;
    }
    return nullptr;
}

void Method::checkAndCreateDefaultBasicBlock()
{
    if(basicBlocks.empty())
    {
        // in case the input code does not always add a label to the start of a function
        basicBlocks.emplace_back(
            *this, new intermediate::BranchLabel(*createLocal(TYPE_LABEL, BasicBlock::DEFAULT_BLOCK)));
        updateCFGOnBlockInsertion(&basicBlocks.back());
    }
}

void Method::updateCFGOnBlockInsertion(BasicBlock* block)
{
    if(!cfg)
        return;
    cfg->updateOnBlockInsertion(*this, *block);
}

void Method::updateCFGOnBlockRemoval(BasicBlock* block)
{
    if(!cfg)
        return;
    cfg->updateOnBlockRemoval(*this, *block);
}

void Method::updateCFGOnBranchInsertion(InstructionWalker it)
{
    if(!cfg)
        return;
    cfg->updateOnBranchInsertion(*this, it);
}

void Method::updateCFGOnBranchRemoval(BasicBlock& affectedBlock, const FastSet<const Local*>& branchTargets)
{
    if(!cfg)
        return;
    cfg->updateOnBranchRemoval(*this, affectedBlock, branchTargets);
}

void Method::addLocalData(Local& loc)
{
    if(loc.type.isSimpleType() && loc.type.getScalarBitCount() > 32 && loc.type.getScalarBitCount() <= 64)
    {
        auto elementType = TYPE_INT32.toVectorType(loc.type.getVectorWidth());
        auto lower = locals.emplace(Local(elementType, loc.name + ".lower")).first;
        auto upper = locals.emplace(Local(elementType, loc.name + ".upper")).first;
        loc.set(MultiRegisterData(&*lower, &*upper));
    }
}
