/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "Eliminator.h"

#include "../InstructionWalker.h"
#include "../Profiler.h"
#include "../analysis/AvailableExpressionAnalysis.h"
#include "../normalization/LiteralValues.h"
#include "../periphery/SFU.h"
#include "log.h"

#include <algorithm>
#include <list>
#include <map>

using namespace vc4c;
using namespace vc4c::optimizations;

bool optimizations::eliminateDeadCode(const Module& module, Method& method, const Configuration& config)
{
    // TODO (additionally or instead of this) walk through locals, check whether they are never read and writings have
    // no side-effects  then walk through all writings of such locals and remove them (example:
    // ./testing/test_vpm_write.cl)
    bool hasChanged = false;
    auto it = method.walkAllInstructions();
    while(!it.isEndOfMethod())
    {
        intermediate::IntermediateInstruction* instr = it.get();
        intermediate::Operation* op = it.get<intermediate::Operation>();
        intermediate::MoveOperation* move = it.get<intermediate::MoveOperation>();
        intermediate::LoadImmediate* load = it.get<intermediate::LoadImmediate>();
        auto address = it.get<intermediate::CodeAddress>();

        // fail-fast on all not-supported instruction types
        // also skip all instructions writing to non-locals (registers)
        if((op || move || load || address) && instr->checkOutputLocal())
        {
            // check whether the output of an instruction is never read
            // only check for ALU-operations and loads, if no flags are set and no special signals are sent
            if(!instr->hasSideEffects())
            {
                const Local* dest = instr->getOutput()->local();
                // check whether local is
                // a) no parameter ??
                if(!dest->is<Parameter>())
                {
                    // b) never read at all
                    // must check from the start, because in SPIR-V, locals can be read before they are written to (e.g.
                    // in phi-node and branch backwards)
                    if(!dest->hasUsers(LocalUse::Type::READER))
                    {
                        CPPLOG_LAZY(logging::Level::DEBUG,
                            log << "Removing instruction " << instr->to_string() << ", since its output is never read"
                                << logging::endl);
                        it.erase();
                        // if we removed this instruction, maybe the previous one can be removed too??
                        it.previousInBlock();
                        hasChanged = true;
                        continue;
                    }
                }
            }
            if(move != nullptr)
            {
                if(move->getSource().checkLocal() && move->getOutput()->checkLocal() && move->isSimpleMove())
                {
                    // if for a move, neither the input-local nor the output-local are written to afterwards,
                    // XXX or the input -local is only written after the last use of the output-local
                    // both locals can be the same and the move can be removed

                    auto inLoc = move->getSource().local();
                    auto outLoc = move->getOutput()->local();
                    // for instruction added by phi-elimination, the result could have been written to (with a different
                    // source) previously, so check
                    if(!outLoc->hasUsers(LocalUse::Type::WRITER) && inLoc->type == outLoc->type)
                    {
                        // TODO what if both locals are written before (and used differently), possible??
                        CPPLOG_LAZY(logging::Level::DEBUG,
                            log << "Merging locals " << inLoc->to_string() << " and " << outLoc->to_string()
                                << " since they contain the same value" << logging::endl);
                        outLoc->forUsers(LocalUse::Type::READER, [inLoc, outLoc](const LocalUser* instr) -> void {
                            // change outLoc to inLoc
                            bool outLocFound = false;
                            for(std::size_t i = 0; i < instr->getArguments().size(); ++i)
                            {
                                Value tmp = instr->assertArgument(i);
                                if(tmp.hasLocal(outLoc))
                                {
                                    tmp = Value(inLoc, tmp.type);
                                    const_cast<LocalUser*>(instr)->setArgument(i, std::move(tmp));
                                    outLocFound = true;
                                }
                            }
                            if(!outLocFound)
                            {
                                throw CompilationError(
                                    CompilationStep::OPTIMIZER, "Unsupported case of instruction merging!");
                            }
                        });
                        // skip ++it, so next instructions is looked at too
                        it.erase();
                        hasChanged = true;
                        continue;
                    }
                }
                else if(move->getSource().hasRegister(REG_UNIFORM) && !move->getSignal().hasSideEffects())
                {
                    // if the added work-group info UNIFORMs are never read, we can remove them (and their flag)
                    auto dest = instr->getOutput()->local()->as<BuiltinLocal>();
                    if(dest && !dest->hasUsers(LocalUse::Type::READER))
                    {
                        using Type = BuiltinLocal::Type;
                        using FuncType = decltype(&KernelUniforms::setGlobalDataAddressUsed);
                        FuncType disableFunc = nullptr;
                        if(dest->builtinType == Type::WORK_DIMENSIONS)
                            disableFunc = &KernelUniforms::setWorkDimensionsUsed;
                        else if(dest->builtinType == Type::GLOBAL_DATA_ADDRESS)
                            disableFunc = &KernelUniforms::setGlobalDataAddressUsed;
                        else if(dest->builtinType == Type::GLOBAL_OFFSET_X)
                            disableFunc = &KernelUniforms::setGlobalOffsetXUsed;
                        else if(dest->builtinType == Type::GLOBAL_OFFSET_Y)
                            disableFunc = &KernelUniforms::setGlobalOffsetYUsed;
                        else if(dest->builtinType == Type::GLOBAL_OFFSET_Z)
                            disableFunc = &KernelUniforms::setGlobalOffsetZUsed;
                        else if(dest->builtinType == Type::GROUP_ID_X)
                            disableFunc = &KernelUniforms::setGroupIDXUsed;
                        else if(dest->builtinType == Type::GROUP_ID_Y)
                            disableFunc = &KernelUniforms::setGroupIDYUsed;
                        else if(dest->builtinType == Type::GROUP_ID_Z)
                            disableFunc = &KernelUniforms::setGroupIDZUsed;
                        else if(dest->builtinType == Type::LOCAL_IDS)
                            disableFunc = &KernelUniforms::setLocalIDsUsed;
                        else if(dest->builtinType == Type::LOCAL_SIZES)
                            disableFunc = &KernelUniforms::setLocalSizesUsed;
                        else if(dest->builtinType == Type::NUM_GROUPS_X)
                            disableFunc = &KernelUniforms::setNumGroupsXUsed;
                        else if(dest->builtinType == Type::NUM_GROUPS_Y)
                            disableFunc = &KernelUniforms::setNumGroupsYUsed;
                        else if(dest->builtinType == Type::NUM_GROUPS_Z)
                            disableFunc = &KernelUniforms::setNumGroupsZUsed;

                        if(disableFunc)
                        {
                            CPPLOG_LAZY(logging::Level::DEBUG,
                                log << "Removing read of work-group UNIFORM, since it is never used: "
                                    << move->to_string() << logging::endl);
                            // disable work-group UNIFORM from method
                            (method.metaData.uniformsUsed.*disableFunc)(false);
                            it.erase();
                            hasChanged = true;
                            continue;
                        }
                    }
                }
            }
        }
        // remove unnecessary writes to special purpose registers
        if((op || move || load || address) && instr->checkOutputRegister() && !instr->hasSideEffects())
        {
            // check whether the register output is actually used. This depends on the kind of register
            // Having an unused rotation offset write can happen, e.g. if the value is zero and the rotation gets
            // rewritten to a move (in #combineVectorRotations)
            bool isUsed = true;
            if(instr->writesRegister(REG_ACC5) || instr->writesRegister(REG_REPLICATE_QUAD) ||
                instr->writesRegister(REG_REPLICATE_ALL))
            {
                auto checkIt = it.copy().nextInBlock();
                while(!checkIt.isEndOfBlock())
                {
                    if(checkIt.get() &&
                        (checkIt->writesRegister(REG_ACC5) || checkIt->writesRegister(REG_REPLICATE_QUAD) ||
                            checkIt->writesRegister(REG_REPLICATE_ALL)))
                    {
                        // register is written before it is read!
                        isUsed = false;
                        break;
                    }
                    if(checkIt.get() &&
                        (checkIt->readsRegister(REG_ACC5) || checkIt->readsRegister(REG_REPLICATE_QUAD) ||
                            checkIt->readsRegister(REG_REPLICATE_ALL)))
                    {
                        // register is used
                        break;
                    }
                    checkIt.nextInBlock();
                }
            }
            // TODO same for SFU/TMU?! Or do they always trigger side effects?

            if(!isUsed)
            {
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Removing write to special purpose register which is never used: " << it->to_string()
                        << logging::endl);
                it.erase();
                hasChanged = true;
                continue;
            }
        }
        // remove unnecessary writes which are immediately overwritten
        if((op || move || load || address) && instr->checkOutputLocal() && !instr->hasSideEffects())
        {
            auto loc = instr->checkOutputLocal();
            auto checkIt = it.copy().nextInBlock();
            while(!checkIt.isEndOfBlock())
            {
                if(checkIt.has() && checkIt->readsLocal(loc))
                    break;
                if(checkIt.has() && checkIt->writesLocal(loc) && !checkIt->hasConditionalExecution())
                    break;
                checkIt.nextInBlock();
            }
            if(!checkIt.isEndOfBlock() && !checkIt->readsLocal(loc))
            {
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Removing write to local which is overridden before the next read: " << it->to_string()
                        << logging::endl);
                it.erase();
                hasChanged = true;
                continue;
            }
        }
        it.nextInMethod();
    }
    // remove unused locals. This is actually not required, but gives us some feedback about the effect of this
    // optimization
    method.cleanLocals();
    return hasChanged;
}

InstructionWalker optimizations::simplifyOperation(
    const Module& module, Method& method, InstructionWalker it, const Configuration& config)
{
    // TODO move to OpCode? As more powerful version of the calculation operator. Use properties where applicable and
    // precalculate where possible
    // Use new solving/simplification here and as replacement of/in precalculate?
    if(auto op = it.get<intermediate::Operation>())
    {
        if(op->isSimpleOperation())
        {
            // TODO could actually allow for setflags! At least replacing, not removing
            // improve by pre-calculating first and second arguments
            const Value firstArg = (op->getFirstArg().getSingleWriter() != nullptr ?
                    op->getFirstArg().getSingleWriter()->precalculate(3).first :
                    NO_VALUE)
                                       .value_or(op->getFirstArg());

            Optional<Value> secondArg = op->getSecondArg();
            if(auto writer = (secondArg & &Value::getSingleWriter))
                secondArg = writer->precalculate(3).first | secondArg;

            Optional<Value> rightIdentity = op->op.getRightIdentity();
            Optional<Value> leftIdentity = op->op.getLeftIdentity();
            Optional<Value> rightAbsorbing = op->op.getRightAbsorbingElement();
            Optional<Value> leftAbsorbing = op->op.getLeftAbsorbingElement();

            // one of the operands is the absorbing element, operation can be replaced with move
            if(leftAbsorbing && firstArg.hasLiteral(leftAbsorbing->getLiteralValue().value()))
            {
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Replacing obsolete " << op->to_string() << " with move 1" << logging::endl);
                it.reset((new intermediate::MoveOperation(
                              op->getOutput().value(), leftAbsorbing.value(), op->getCondition(), op->getFlags()))
                             ->addDecorations(it->decoration));
            }
            else if(rightAbsorbing && secondArg && secondArg->hasLiteral(rightAbsorbing->getLiteralValue().value()))
            {
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Replacing obsolete " << op->to_string() << " with move 2" << logging::endl);
                it.reset((new intermediate::MoveOperation(
                              op->getOutput().value(), rightAbsorbing.value(), op->getCondition(), op->getFlags()))
                             ->addDecorations(it->decoration));
            }
            // both operands are the same and the operation is self-inverse <=> f(a, a) = 0
            else if(op->op.isSelfInverse() && firstArg == secondArg && firstArg.type.getElementType() != TYPE_BOOL)
            {
                // do not replace xor true, true, since this is almost always combined with or true, true for inverted
                // condition
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Replacing obsolete " << op->to_string() << " with move 7" << logging::endl);
                it.reset((new intermediate::MoveOperation(op->getOutput().value(),
                              Value(Literal(0u), op->getOutput()->type), op->getCondition(), op->getFlags()))
                             ->addDecorations(it->decoration));
            }
            // writes into the input -> can be removed, if it doesn't do anything
            else if(op->getOutput() && op->getOutput().value() == op->getFirstArg())
            {
                // check whether second-arg exists and does nothing
                if(rightIdentity && secondArg && secondArg->hasLiteral(rightIdentity->getLiteralValue().value()))
                {
                    CPPLOG_LAZY(logging::Level::DEBUG, log << "Removing obsolete " << op->to_string() << logging::endl);
                    it.erase();
                    // don't skip next instruction
                    it.previousInBlock();
                }
                else if(op->op.isIdempotent() && secondArg && secondArg.value() == firstArg)
                {
                    CPPLOG_LAZY(logging::Level::DEBUG, log << "Removing obsolete " << op->to_string() << logging::endl);
                    it.erase();
                    // don't skip next instruction
                    it.previousInBlock();
                }
            }
            else if(op->getOutput() && op->getSecondArg() && op->getOutput().value() == op->assertArgument(1))
            {
                // check whether first-arg does nothing
                if(leftIdentity && firstArg.hasLiteral(leftIdentity->getLiteralValue().value()))
                {
                    CPPLOG_LAZY(logging::Level::DEBUG, log << "Removing obsolete " << op->to_string() << logging::endl);
                    it.erase();
                    // don't skip next instruction
                    it.previousInBlock();
                }
                else if(op->op.isIdempotent() && secondArg && secondArg.value() == firstArg &&
                    !firstArg.checkRegister() && !firstArg.isUndefined())
                {
                    CPPLOG_LAZY(logging::Level::DEBUG, log << "Removing obsolete " << op->to_string() << logging::endl);
                    it.erase();
                    // don't skip next instruction
                    it.previousInBlock();
                }
            }
            else // writes to another local -> can be replaced with move
            {
                // check whether second argument exists and does nothing
                if(rightIdentity && secondArg && secondArg->hasLiteral(rightIdentity->getLiteralValue().value()))
                {
                    CPPLOG_LAZY(logging::Level::DEBUG,
                        log << "Replacing obsolete " << op->to_string() << " with move 3" << logging::endl);
                    it.reset((new intermediate::MoveOperation(
                                  op->getOutput().value(), op->getFirstArg(), op->getCondition(), op->getFlags()))
                                 ->addDecorations(it->decoration));
                }
                // check whether first argument does nothing
                else if(leftIdentity && secondArg && firstArg.hasLiteral(leftIdentity->getLiteralValue().value()))
                {
                    CPPLOG_LAZY(logging::Level::DEBUG,
                        log << "Replacing obsolete " << op->to_string() << " with move 4" << logging::endl);
                    it.reset((new intermediate::MoveOperation(
                                  op->getOutput().value(), op->assertArgument(1), op->getCondition(), op->getFlags()))
                                 ->addDecorations(it->decoration));
                }
                // check whether operation does not really calculate anything
                else if(op->op.isIdempotent() && secondArg && secondArg.value() == firstArg &&
                    !firstArg.checkRegister() && !firstArg.isUndefined())
                {
                    logging::logLazy(logging::Level::DEBUG, [&]() {
                        logging::debug() << secondArg.value().to_string() << " - " << firstArg.to_string()
                                         << logging::endl;
                        logging::debug() << "Replacing obsolete " << op->to_string() << " with move 5" << logging::endl;
                    });
                    it.reset((new intermediate::MoveOperation(
                                  op->getOutput().value(), op->assertArgument(1), op->getCondition(), op->getFlags()))
                                 ->addDecorations(it->decoration));
                }
                else if(op->op == OP_XOR && op->getFirstArg().getLiteralValue() == Literal(-1))
                {
                    // LLVM converts ~%a to %a xor -1, we convert it back to free the local from use-with-literal
                    CPPLOG_LAZY(logging::Level::DEBUG,
                        log << "Replacing XOR " << op->to_string() << " with NOT" << logging::endl);
                    it.reset((new intermediate::Operation(OP_NOT, op->getOutput().value(), op->getSecondArg().value(),
                                  op->getCondition(), op->getFlags()))
                                 ->addDecorations(it->decoration));
                }
                else if(op->op == OP_XOR && (op->getSecondArg() & &Value::getLiteralValue) == Literal(-1))
                {
                    CPPLOG_LAZY(logging::Level::DEBUG,
                        log << "Replacing XOR " << op->to_string() << " with NOT" << logging::endl);
                    it.reset((new intermediate::Operation(OP_NOT, op->getOutput().value(), op->getFirstArg(),
                                  op->getCondition(), op->getFlags()))
                                 ->addDecorations(it->decoration));
                }
            }
        }
        // TODO trunc to int32/float
    }
    else if(auto move = it.get<intermediate::MoveOperation>())
    {
        if(move->getSource() == move->getOutput().value() && move->isSimpleMove())
        {
            // skip copying to same, if no flags/signals/pack and unpack-modes are set
            CPPLOG_LAZY(logging::Level::DEBUG, log << "Removing obsolete " << move->to_string() << logging::endl);
            it.erase();
            // don't skip next instruction
            it.previousInBlock();
        }
        if(it->getVectorRotation() && move->getSource().isAllSame())
        {
            // replace rotation of splat value with move
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Replacing obsolete " << move->to_string() << " with move 6" << logging::endl);
            it.reset((new intermediate::MoveOperation(
                          move->getOutput().value(), move->getSource(), move->getCondition(), move->getFlags()))
                         ->addDecorations(it->decoration));
        }
    }

    return it;
}

InstructionWalker optimizations::foldConstants(
    const Module& module, Method& method, InstructionWalker it, const Configuration& config)
{
    intermediate::Operation* op = it.get<intermediate::Operation>();
    // Don't pre-calculate on flags, since i.e. carry flags cannot be set by moves!
    // Similarly for (un)pack modes (esp. 32-bit saturation) XXX we could precalculate them in the compiler
    if(op != nullptr && !op->doesSetFlag() && !op->hasUnpackMode() && !op->hasPackMode())
    {
        // calculations with literals can be pre-calculated
        if((op->getFirstArg().getConstantValue() & &Value::getLiteralValue) &&
            (!op->getSecondArg() || (op->assertArgument(1).getConstantValue() & &Value::getLiteralValue)))
        {
            if(op->hasConditionalExecution() && op->op == OP_XOR && op->getSecondArg() == op->getFirstArg())
            {
                // skip "xor ?, true, true", so it can be optimized (combined with "move ?, true") afterwards
                // also skip any "xor ?, val, val", since they are created on purpose (by combineSelectionWithZero to
                // allow for combination with the other case)
                return it;
            }
            if(op->hasDecoration(intermediate::InstructionDecorations::CONSTANT_LOAD))
            {
                // This instruction was inserted for the purpose of loading the constant value, don't revert that.
                // Otherwise, we will probably revert this precalculation back to the same constant load instruction in
                // the adjustment step.
                return it;
            }
            if(auto value = op->precalculate(3).first)
            {
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Replacing '" << op->to_string() << "' with constant value: " << value.to_string()
                        << logging::endl);
                it.reset((new intermediate::MoveOperation(op->getOutput().value(), value.value()))->copyExtrasFrom(op));
            }
        }
    }
    return it;
}

InstructionWalker optimizations::eliminateReturn(
    const Module& module, Method& method, InstructionWalker it, const Configuration& config)
{
    if(it.get<intermediate::Return>())
    {
        auto target = method.findBasicBlock(BasicBlock::LAST_BLOCK);
        if(!target)
            target = &method.createAndInsertNewBlock(method.end(), BasicBlock::LAST_BLOCK);

        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Replacing return in kernel-function with branch to end-label" << logging::endl);
        it.reset(new intermediate::Branch(target->getLabel()->getLabel()));
    }
    return it;
}

static bool isNoReadBetween(InstructionWalker first, InstructionWalker second, Register reg)
{
    first.nextInBlock();
    while(!first.isEndOfBlock() && first != second)
    {
        // just to be sure (e.g. for reading TMU/SFU/VPM), check triggering load of r4 and releasing of mutex too
        if(first->readsRegister(reg) || first->writesRegister(reg) || first->getSignal().triggersReadOfR4() ||
            first->writesRegister(REG_MUTEX))
            return false;
        // for reading VPM, check also VPM read setup
        if(reg.isVertexPipelineMemory() && (first->checkOutputRegister() & &Register::isVertexPipelineMemory))
            return false;
        first.nextInBlock();
    }
    return true;
}

static std::function<bool(const intermediate::IntermediateInstruction&)> createRegisterCheck(
    InstructionWalker it, const Value& src)
{
    auto reg = src.checkRegister();
    if(!reg)
        // no register read, this check does not apply
        return [](const auto&) -> bool { return true; };
    /*
     * We need to make sure, that the register is not overwritten between the original register read and the read after
     * this optimization is applied.
     */
    if(*reg == REG_SFU_OUT || *reg == REG_TMU_OUT)
    {
        return [](const auto& inst) -> bool {
            // allow as long as r4 accumulator is not written
            return !(inst.checkOutputRegister() & &Register::triggersReadOfR4) && !inst.getSignal().triggersReadOfR4();
        };
    }
    if(*reg == REG_ACC5)
    {
        return [](const auto& inst) -> bool {
            // allow as long as neither r5 nor any replication registers are written
            auto outReg = inst.checkOutputRegister();
            return !outReg || (*outReg != REG_ACC5 && *outReg != REG_REPLICATE_ALL && *outReg != REG_REPLICATE_QUAD);
        };
    }
    // any other register, do not allow to move, since we did not run the proper check
    return [](const auto& inst) -> bool { return false; };
}

/* TODO
 * this propagation should work among basic blocks.
 * but we need very keen to unsafe-case
 *
 *     A    Move propagation of an instruction in C may dangerous if an instruction in D is rewritten.
 *    / \   But, the propagation A to B and C should work.
 *   /   \
 *  B    C
 *  \    /
 *   \  /
 *    D
 */
bool optimizations::propagateMoves(const Module& module, Method& method, const Configuration& config)
{
    auto it = method.walkAllInstructions();
    auto replaced = false;
    auto groupIdsLocal = method.findBuiltin(BuiltinLocal::Type::GROUP_IDS);
    while(!it.isEndOfMethod())
    {
        auto const op = it.get<intermediate::MoveOperation>();

        // just copy of value
        // should not work like:
        //
        // - mov.setf null, r0
        // - mov r0, r1 with pack, unpack
        // - mov r0, r4 // TODO r4 can be propagate unless signal or the use of sfu isn't issued
        // - mov r0, r5
        // - mov r0, vpm
        // - mov r0, unif
        //
        // very side-effects are mattered here.
        //
        // - mov.setf r0, r1
        // - mov r0, r1, load_tmu0
        if(op && !it->getVectorRotation() && !op->hasConditionalExecution() && !op->hasPackMode() &&
            !op->hasUnpackMode() && op->getOutput().has_value() &&
            (!op->getSource().checkRegister() || !op->getSource().reg().hasSideEffectsOnRead()) &&
            (!op->checkOutputRegister()) &&
            (!op->readsLiteral() || normalization::toImmediate(*op->getSource().getLiteralValue())) &&
            /* XXX for now skip %group_ids, since we otherwise screw up our handcrafted code in the work-group loop */
            (!groupIdsLocal || !op->readsLocal(groupIdsLocal)))
        {
            auto it2 = it.copy().nextInBlock();
            auto oldValue = op->getOutput().value();
            const auto& newValue = op->getSource();
            // only continue iterating as long as there is a read of the local left
            FastSet<const LocalUser*> remainingLocalReads = oldValue.checkLocal() ?
                oldValue.local()->getUsers(LocalUse::Type::READER) :
                FastSet<const LocalUser*>{};
            // registers fixed to physical file B cannot be combined with literal
            bool skipLiteralReads = newValue.checkRegister() && newValue.reg().file == RegisterFile::PHYSICAL_B;
            auto checkRegister = createRegisterCheck(it, newValue);
            while(!it2.isEndOfBlock() && !remainingLocalReads.empty())
            {
                bool replacedThisInstruction = false;
                if(!skipLiteralReads || !it2->readsLiteral())
                {
                    for(auto arg : it2->getArguments())
                    {
                        if(arg == oldValue && !arg.checkLiteral() && !arg.checkImmediate())
                        {
                            replaced = true;
                            replacedThisInstruction = true;
                            it2->replaceValue(oldValue, newValue, LocalUse::Type::READER);
                            remainingLocalReads.erase(it2.get());
                        }
                    }
                }

                if(replacedThisInstruction)
                    foldConstants(module, method, it2, config);

                if(it2->getOutput() && it2->getOutput() == oldValue)
                    break;

                if(it2.has() && !checkRegister(*it2.get()))
                    break;

                it2.nextInBlock();
            }
        }

        it.nextInMethod();
    }

    return replaced;
}

static bool canMoveInstruction(InstructionWalker source, InstructionWalker destination)
{
    // don't move reading/writing of r5 over other reading/writing of r5
    auto checkReplication = [](InstructionWalker it) {
        return it->readsRegister(REG_ACC5) || it->readsRegister(REG_REPLICATE_ALL) ||
            it->readsRegister(REG_REPLICATE_QUAD) || it->writesRegister(REG_ACC5) ||
            it->writesRegister(REG_REPLICATE_ALL) || it->writesRegister(REG_REPLICATE_QUAD);
    };
    bool checkForReplicationRegister = checkReplication(source);
    auto it = source.copy().nextInBlock();
    while(!it.isEndOfBlock() && it != destination)
    {
        if(checkForReplicationRegister && checkReplication(it))
            return false;
        it.nextInBlock();
    }
    return true;
}

bool optimizations::eliminateRedundantMoves(const Module& module, Method& method, const Configuration& config)
{
    /*
     * XXX can be improved to move UNIFORM reads,
     * iff in same/first block and no reorder of UNIFORM values/UNIFORM pointer is not re-set.
     * Problem: initially there are reads of UNIFORM between write and usage, even if they could also be removed
     * -> would need to run this optimization from end-to-front (additionally to front-to-end?)
     *
     * behavior can be tested on CTS: api/test_api min_max_constant_args
     */

    bool codeChanged = false;
    auto it = method.walkAllInstructions();
    while(!it.isEndOfMethod())
    {
        if(it.get<intermediate::MoveOperation>() &&
            !it->hasDecoration(intermediate::InstructionDecorations::PHI_NODE) && !it->hasPackMode() &&
            !it->hasUnpackMode() && !it->hasConditionalExecution() && !it->getVectorRotation())
        {
            // skip PHI-nodes, since they are read in another basic block (and the output is written more than once
            // anyway) as well as modification of the value, conditional execution and vector-rotations
            const intermediate::MoveOperation* move = it.get<intermediate::MoveOperation>();

            // the source is written and read only once
            bool sourceUsedOnce = move->getSource().getSingleWriter() != nullptr &&
                move->getSource().local()->countUsers(LocalUse::Type::READER) == 1;
            // the destination is written and read only once (and not in combination with a literal value, to not
            // introduce register conflicts)
            bool destUsedOnce = move->checkOutputLocal() && move->getOutput()->getSingleWriter() == move &&
                move->getOutput()->local()->countUsers(LocalUse::Type::READER) == 1;
            bool destUsedOnceWithoutLiteral = destUsedOnce &&
                !(*move->getOutput()->local()->getUsers(LocalUse::Type::READER).begin())->readsLiteral();

            auto sourceWriter = (move->getSource().getSingleWriter() != nullptr) ?
                it.getBasicBlock()->findWalkerForInstruction(move->getSource().getSingleWriter(), it) :
                Optional<InstructionWalker>{};
            auto destinationReader =
                (move->checkOutputLocal() && move->getOutput()->local()->countUsers(LocalUse::Type::READER) == 1) ?
                it.getBasicBlock()->findWalkerForInstruction(
                    *move->getOutput()->local()->getUsers(LocalUse::Type::READER).begin(),
                    it.getBasicBlock()->walkEnd()) :
                Optional<InstructionWalker>{};
            if(move->getSource() == move->getOutput().value() &&
                !move->hasOtherSideEffects(intermediate::SideEffectType::SIGNAL))
            {
                if(move->getSignal() == SIGNAL_NONE)
                {
                    CPPLOG_LAZY(
                        logging::Level::DEBUG, log << "Removing obsolete move: " << it->to_string() << logging::endl);
                    it.erase();
                    // don't skip next instruction
                    it.previousInBlock();
                    codeChanged = true;
                }
                else
                {
                    CPPLOG_LAZY(logging::Level::DEBUG,
                        log << "Removing obsolete move with nop: " << it->to_string() << logging::endl);
                    it.reset(new intermediate::Nop(intermediate::DelayType::WAIT_REGISTER, move->getSignal()));
                    codeChanged = true;
                }
            }

            else if(!it->hasSideEffects() && sourceUsedOnce && destUsedOnceWithoutLiteral && destinationReader &&
                move->getSource().type == move->getOutput()->type)
            {
                // if the source is written only once and the destination is read only once, we can replace the uses of
                // the output with the input
                // XXX we need to check the type equality, since otherwise Reordering might re-order the reading before
                // the writing (if the local is written as type A and read as type B)
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Removing obsolete move by replacing uses of the output with the input: " << it->to_string()
                        << logging::endl);
                (*destinationReader)
                    ->replaceValue(move->getOutput().value(), move->getSource(), LocalUse::Type::READER);
                if((*destinationReader).get<intermediate::MoveOperation>())
                    (*destinationReader)->addDecorations(intermediate::forwardDecorations(it->decoration));
                it.erase();
                // to not skip the next instruction
                it.previousInBlock();
                codeChanged = true;
            }
            else if(it->checkOutputRegister() && sourceUsedOnce && sourceWriter &&
                (!(*sourceWriter)->hasSideEffects()
                    // FIXME this re-orders UNIFORM reads (e.g. in test_branches.cl) ||
                    // !((*sourceWriter)->signal.hasSideEffects() || (*sourceWriter)->doesSetFlag()))
                    ) &&
                !it->getSignal().hasSideEffects() && canMoveInstruction(*sourceWriter, it) &&
                // TODO don't know why this does not work (maybe because of some other optimization applied to the
                // result?), but rewriting moves to rotation registers screw up the
                // TestVectorFunctions#testShuffle2Vector16 test
                !it->writesRegister(REG_REPLICATE_ALL) && !it->writesRegister(REG_REPLICATE_QUAD) &&
                // Registers with side-effects are peripheral and cannot be written conditionally
                (!it->checkOutputRegister()->hasSideEffectsOnWrite() || !(*sourceWriter)->hasConditionalExecution()) &&
                // Registers with side-effects are peripheral and cannot be packed into
                (!it->checkOutputRegister()->hasSideEffectsOnWrite() || !(*sourceWriter)->hasPackMode()))
            {
                // if the source is only used once (by this move) and the destination is a register, we can replace this
                // move by the operation calculating the source  This optimization can save almost one instruction per
                // VPM write/VPM address write
                // TODO This could potentially lead to far longer usage-ranges for operands of sourceWriter and
                // therefore to register conflicts
                // TODO when replacing moves which set flags, need to make sure, flags are not overridden in between!
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Replacing obsolete move with instruction calculating its source: " << it->to_string()
                        << logging::endl);
                auto output = it->getOutput();
                auto setFlags = it->doesSetFlag();
                auto sourceDecorations = intermediate::forwardDecorations((*sourceWriter)->decoration);
                it.reset(sourceWriter->release());
                sourceWriter->erase();
                it->setOutput(std::move(output));
                if(auto extended = it.get<intermediate::ExtendedInstruction>())
                    extended->setSetFlags(setFlags ? SetFlag::SET_FLAGS : SetFlag::DONT_SET);
                it->addDecorations(sourceDecorations);
                codeChanged = true;
            }
            else if(move->getSource().checkRegister() && destUsedOnce &&
                (destUsedOnceWithoutLiteral || has_flag(move->getSource().reg().file, RegisterFile::PHYSICAL_ANY) ||
                    has_flag(move->getSource().reg().file, RegisterFile::ACCUMULATOR)) &&
                destinationReader && !move->getSignal().hasSideEffects() && !move->doesSetFlag() &&
                !(*destinationReader)->hasUnpackMode() && !(*destinationReader)->hasConditionalExecution() &&
                !(*destinationReader)->readsRegister(move->getSource().reg()) &&
                isNoReadBetween(it, destinationReader.value(), move->getSource().reg()) &&
                /* Tests have shown that an instruction cannot read and write VPM at the same time */
                (!move->getSource().hasRegister(REG_VPM_IO) ||
                    !(*destinationReader)->getOutput()->hasRegister(REG_VPM_IO)))
            {
                // if the source is a register, the output is only used once, this instruction has no signals/sets no
                // flags, the output consumer does not also read this move's source and there is no read of the source
                // between the move and the consumer, the consumer can directly use the register moved here
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Replacing obsolete move by inserting the source into the instruction consuming its result: "
                        << it->to_string() << logging::endl);
                const Value newInput(move->getSource().reg(), move->getOutput()->type);
                const Local* oldLocal = move->getOutput()->local();
                for(std::size_t i = 0; i < (*destinationReader)->getArguments().size(); ++i)
                {
                    if((*destinationReader)->assertArgument(i).hasLocal(oldLocal))
                        (*destinationReader)->setArgument(i, newInput);
                }
                if((*destinationReader)->checkOutputLocal() &&
                    !Local::getLocalData<LocalData>((*destinationReader)->checkOutputLocal()))
                {
                    if(oldLocal->residesInMemory())
                        const_cast<Local*>((*destinationReader)->checkOutputLocal())
                            ->set(ReferenceData(*oldLocal, ANY_ELEMENT));
                    else if(auto data = oldLocal->get<ReferenceData>())
                        const_cast<Local*>((*destinationReader)->checkOutputLocal())->set(ReferenceData(*data));
                }
                it.erase();
                // to not skip the next instruction
                it.previousInBlock();
                codeChanged = true;
            }
        }
        it.nextInMethod();
    }

    return codeChanged;
}

static bool canReplaceBitOp(const intermediate::Operation& op)
{
    return !op.hasUnpackMode() && !has_flag(op.getSideEffects(), intermediate::SideEffectType::REGISTER_READ);
}

static bool hasByteExtractionMode(const intermediate::UnpackingInstruction& inst)
{
    return inst.getUnpackMode() == UNPACK_8A_32 || inst.getUnpackMode() == UNPACK_8B_32 ||
        inst.getUnpackMode() == UNPACK_8C_32 || inst.getUnpackMode() == UNPACK_8D_32;
}

static bool hasSingleByteExtractionWriter(const Value& val)
{
    auto writer =
        dynamic_cast<const intermediate::MoveOperation*>((check(val.checkLocal()) & &Local::getSingleWriter).get());
    return writer && !writer->getVectorRotation() && !writer->hasConditionalExecution() && !writer->hasPackMode() &&
        hasByteExtractionMode(*writer);
}

bool optimizations::eliminateRedundantBitOp(const Module& module, Method& method, const Configuration& config)
{
    // See https://en.wikipedia.org/wiki/Boolean_algebra#Monotone_laws
    bool replaced = false;
    auto it = method.walkAllInstructions();
    while(!it.isEndOfMethod())
    {
        auto op = it.get<intermediate::Operation>();
        if(op && !op->hasConditionalExecution())
        {
            if(op->op == OP_AND)
            {
                // and v1, v2, v3 => and v1, v2, v4
                // and v4, v1, v2    mov v4, v1
                //
                // and v1, v2, v3 => and v1, v2, v3
                // or  v4, v1, v2    mov v4, v2
                auto foundAnd = [&](const Local* out, const Local* in, InstructionWalker walker) {
                    auto it = walker.copy().nextInBlock();
                    // have some kind of upper limit for number if instructions to check
                    auto instructionsRemaining = config.additionalOptions.maxCommonExpressionDinstance;
                    while(instructionsRemaining > 0 && !it.isEndOfBlock())
                    {
                        --instructionsRemaining;
                        auto op2 = it.get<intermediate::Operation>();
                        if(op2 && op2->op == OP_AND && canReplaceBitOp(*op2) && op2->readsLocal(out) &&
                            op2->readsLocal(in))
                        {
                            CPPLOG_LAZY(logging::Level::DEBUG,
                                log << "Replacing (%a AND %b) AND %a with %a AND %b: " << op2->to_string()
                                    << logging::endl);
                            auto mov =
                                new intermediate::MoveOperation(op2->getOutput().value(), out->createReference());
                            mov->copyExtrasFrom(it.get());
                            replaced = true;
                            it.reset(mov);
                        }
                        else if(op2 && op2->op == OP_OR && canReplaceBitOp(*op2) && op2->readsLocal(out) &&
                            op2->readsLocal(in))
                        {
                            CPPLOG_LAZY(logging::Level::DEBUG,
                                log << "Replacing (%a AND %b) OR %a with %a: " << op2->to_string() << logging::endl);
                            auto mov = new intermediate::MoveOperation(op2->getOutput().value(), in->createReference());
                            mov->copyExtrasFrom(it.get());
                            replaced = true;
                            it.reset(mov);
                        }

                        it.nextInBlock();
                    }
                };

                auto out = op->checkOutputLocal();
                if(out && !op->readsLocal(out))
                {
                    if(auto loc = op->getFirstArg().checkLocal())
                        foundAnd(out, loc, it);
                    if(auto loc = op->getSecondArg() & &Value::checkLocal)
                        foundAnd(out, loc, it);
                }

                // %b = %a (zextByteXTo32)
                // %c = %b & 255 -> superfluous
                auto hasByteMask = [](const Value& val) -> bool {
                    auto constVal = val.getConstantValue();
                    return constVal && constVal->hasLiteral(Literal(255));
                };
                auto argIt = std::find_if(op->getArguments().begin(), op->getArguments().end(), hasByteMask);
                if(argIt != op->getArguments().end())
                {
                    auto otherArg = op->findOtherArgument(*argIt);
                    if(otherArg & hasSingleByteExtractionWriter)
                    {
                        logging::debug() << "Replacing redundant byte masking for value already extracted from single "
                                            "byte with move: "
                                         << op->to_string() << logging::endl;
                        replaced = true;
                        it.reset((new intermediate::MoveOperation(*op->getOutput(), *otherArg))->copyExtrasFrom(op));
                    }
                }
            }
            else if(op->op == OP_OR)
            {
                // or  v1, v2, v3 => or  v1, v2, v4
                // and v4, v1, v2    mov v4, v2
                //
                // or  v1, v2, v3 => or  v1, v2, v3
                // or  v4, v1, v2    mov v4, v1
                auto foundOr = [&](const Local* out, const Local* in, InstructionWalker walker) {
                    auto it = walker.copy().nextInBlock();
                    // have some kind of upper limit for number if instructions to check
                    auto instructionsRemaining = config.additionalOptions.maxCommonExpressionDinstance;
                    while(instructionsRemaining > 0 && !it.isEndOfBlock())
                    {
                        --instructionsRemaining;
                        auto op2 = it.get<intermediate::Operation>();
                        if(op2 && op2->op == OP_AND && canReplaceBitOp(*op2) && op2->readsLocal(out) &&
                            op2->readsLocal(in))
                        {
                            CPPLOG_LAZY(logging::Level::DEBUG,
                                log << "Replacing (%a OR %b) AND %a with %a: " << op2->to_string() << logging::endl);
                            auto mov = new intermediate::MoveOperation(op2->getOutput().value(), in->createReference());
                            mov->copyExtrasFrom(it.get());
                            replaced = true;
                            it.reset(mov);
                        }
                        else if(op2 && op2->op == OP_OR && canReplaceBitOp(*op2) && op2->readsLocal(out) &&
                            op2->readsLocal(in))
                        {
                            CPPLOG_LAZY(logging::Level::DEBUG,
                                log << "Replacing (%a OR %b) OR %a with %a OR %b: " << op2->to_string()
                                    << logging::endl);
                            auto mov =
                                new intermediate::MoveOperation(op2->getOutput().value(), out->createReference());
                            mov->copyExtrasFrom(it.get());
                            replaced = true;
                            it.reset(mov);
                        }

                        it.nextInBlock();
                    }
                };

                auto out = op->checkOutputLocal();
                if(out && !op->readsLocal(out))
                {
                    if(auto loc = op->getFirstArg().checkLocal())
                        foundOr(out, loc, it);
                    if(auto loc = op->getSecondArg() & &Value::checkLocal)
                        foundOr(out, loc, it);
                }
            }
            else if(op->op == OP_ASR && !op->doesSetFlag() && !op->hasPackMode())
            {
                /*
                 * %y = asr %x, const1
                 * %z = and %y, const2
                 *
                 * if const2 <= 2^const1:
                 * %y = shr %x, const1
                 * %z = and %y, const2
                 */
                // the mask of bits from the input which are only shifted, not modified. I.e. this is the bit-mask of
                // the result which is not set to leading ones or zeroes.
                uint32_t mask{0xFFFFFFFF};
                if(auto lit = op->getSecondArg() & &Value::getLiteralValue)
                {
                    // only last bits are actually used by ALU, see OP_ASR documentation
                    auto offset = lit->unsignedInt() & 0x1F;
                    offset = 32 - offset;
                    mask = (1u << offset) - 1u;
                }
                auto out = op->checkOutputLocal();
                if(mask != uint32_t{0xFFFFFFFF} && out &&
                    std::all_of(out->getUsers().begin(), out->getUsers().end(),
                        [&](const std::pair<const LocalUser*, LocalUse> user) -> bool {
                            if(!user.second.readsLocal())
                                return true;
                            if(auto userOp = dynamic_cast<const intermediate::Operation*>(user.first))
                            {
                                auto otherArg = userOp->findOtherArgument(*op->getOutput());
                                auto otherLit =
                                    (otherArg ? otherArg->getConstantValue() : NO_VALUE) & &Value::getLiteralValue;
                                return userOp->op == OP_AND && !userOp->hasUnpackMode() && otherLit &&
                                    isPowerTwo(otherLit->unsignedInt() + 1u) && otherLit->unsignedInt() <= mask;
                            }
                            return false;
                        }))
                {
                    // if all of our readers are simple ANDs with a constant mask which covers less or equal bits than
                    // the mask of we calculated, we know that all the sign-extended bits are not used. Therefore the
                    // (actually relevant part of the) result for the ASR is the same as for SHR -> simplify.
                    CPPLOG_LAZY(logging::Level::DEBUG,
                        log << "Replacing arithmetic shift with simpler bit-wise shift: " << op->to_string()
                            << logging::endl);
                    op->op = OP_SHR;
                    replaced = true;
                }
            }
            // we need to recheck the operation, since we might have reset it above
            op = it.get<intermediate::Operation>();
            // fall-through on purpose, since we can improve on the above even further with the check below
            if(op && op->op == OP_SHR && !op->hasUnpackMode() && !op->doesSetFlag())
            {
                /*
                 * %b = shl %a, const1
                 * %c = shr %b, const2
                 *
                 * if const1 == const2:
                 * %c = and %a, 2^const1
                 */
                auto writer = dynamic_cast<const intermediate::Operation*>(op->getFirstArg().getSingleWriter());
                // the mask of bits from the input which are only shifted, not modified. I.e. this is the bit-mask of
                // the result which is not set to leading zeroes.
                uint32_t mask{0xFFFFFFFF};
                if(auto lit = op->getSecondArg() & &Value::getLiteralValue)
                {
                    // only last bits are actually used by ALU, see OP_SHR documentation
                    auto offset = lit->unsignedInt() & 0x1F;
                    offset = 32 - offset;
                    mask = offset == 32 ? 0xFFFFFFFF : (1u << offset) - 1u;
                }
                if(mask != uint32_t{0xFFFFFFFF} && writer && writer->op == OP_SHL && !writer->hasPackMode() &&
                    writer->getSecondArg() &&
                    writer->getSecondArg()->getConstantValue() == op->getSecondArg()->getConstantValue())
                {
                    CPPLOG_LAZY(logging::Level::DEBUG,
                        log << "Replacing redundant left and right shift with same offset to and with mask: "
                            << op->to_string() << logging::endl);
                    auto input = writer->getFirstArg();
                    // (a << x) >> x -> (a & 2^x)
                    op->replaceValue(op->getFirstArg(), input, LocalUse::Type::READER);
                    op->replaceValue(*op->getSecondArg(), Value(Literal(mask), TYPE_INT32), LocalUse::Type::READER);
                    op->op = OP_AND;
                    replaced = true;
                }
            }
        }

        it.nextInMethod();
    }

    return replaced;
}

bool optimizations::eliminateCommonSubexpressions(const Module& module, Method& method, const Configuration& config)
{
    bool replacedSomething = false;
    for(auto& block : method)
    {
        // we do not run the whole analysis in front, but only the next step to save on memory usage
        // For that purpose, we also override the previous expressions on every step
        analysis::AvailableExpressionAnalysis::Cache cache{};
        AvailableExpressions expressions{};
        FastMap<const Local*, std::shared_ptr<Expression>> calculatingExpressions{};

        for(auto it = block.walk(); !it.isEndOfBlock(); it.nextInBlock())
        {
            if(!it.has())
                continue;
            std::shared_ptr<Expression> expr;
            std::tie(expressions, expr) = analysis::AvailableExpressionAnalysis::analyzeAvailableExpressions(
                it.get(), expressions, cache, config.additionalOptions.maxCommonExpressionDinstance);
            if(expr)
            {
                auto newExpr = expr;
                if(auto out = it->checkOutputLocal())
                    // remove from cache before using the result for the expression not to depend on itself
                    calculatingExpressions.erase(out);

                auto exprIt = expressions.find(expr);
                // replace instruction with matching expression, if the expression is not constant (no use replacing
                // loading of constants with copies of a local initialized with a constant)
                if(exprIt != expressions.end() && exprIt->second.first != it.get() && !expr->getConstantExpression())
                {
                    CPPLOG_LAZY(logging::Level::DEBUG,
                        log << "Found common subexpression: " << it->to_string() << " is the same as "
                            << exprIt->second.first->to_string() << logging::endl);
                    it.reset(new intermediate::MoveOperation(
                        it->getOutput().value(), exprIt->second.first->getOutput().value()));
                    replacedSomething = true;
                }
                else if(*(newExpr = expr->combineWith(calculatingExpressions)) != *expr)
                {
                    if(newExpr->insertInstructions(it, it->getOutput().value(), expressions))
                    {
                        CPPLOG_LAZY(logging::Level::WARNING,
                            log << "Rewriting expression '" << expr->to_string() << "' to '" << newExpr->to_string()
                                << "'" << logging::endl);

                        if(exprIt != expressions.end() && exprIt->second.first == it.get())
                            // reset this expression, since the mapped instruction will be overwritten
                            expressions.erase(exprIt);

                        // remove original instruction
                        it.erase();
                        it.previousInBlock();
                        if(auto loc = it->checkOutputLocal())
                            calculatingExpressions.emplace(loc, newExpr);
                        replacedSomething = true;
                        expressions.emplace(newExpr, std::make_pair(it.get(), 0));
                    }
                }

                if(auto out = it->checkOutputLocal())
                    // add to cache after using the result for the expression not to depend on itself
                    // NOTE: not overwriting the above emplace is on purpose
                    calculatingExpressions.emplace(out, expr);
            }
            else if(auto loc = it->checkOutputLocal())
            {
                // if we failed to create an expression for an output local (e.g. because of conditional access, etc.),
                // need to reset the expression for that local, since any previous expression might no longer be
                // accurate.
                calculatingExpressions.erase(loc);
            }
        }
    }
    return replacedSomething;
}

InstructionWalker optimizations::rewriteConstantSFUCall(
    const Module& module, Method& method, InstructionWalker it, const Configuration& config)
{
    if(!it.has() || !(it->checkOutputRegister() & &Register::isSpecialFunctionsUnit))
        return it;
    if(it->hasConditionalExecution())
        // if we write conditionally to SFU, there might be another conditional write to SFU (if this is allowed at
        // all!!)
        return it;
    if(it->hasOtherSideEffects(intermediate::SideEffectType::REGISTER_WRITE))
        // there are other side-effects for this instruction writing into SFU, which we cannot remove
        return it;
    if(it->hasPackMode() || it->hasUnpackMode())
        return it;

    auto constantValue = it->precalculate(3).first;
    if(auto result =
            constantValue ? periphery::precalculateSFU(it->getOutput()->reg(), constantValue.value()) : NO_VALUE)
    {
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Replacing SFU call with constant input '" << it->to_string()
                << "' to move of result: " << result->to_string() << logging::endl);

        // remove this instruction, 2 NOPs (with SFU type) and rewrite the result
        it.erase();
        unsigned numDelays = 2;
        while(numDelays != 0 && !it.isEndOfBlock())
        {
            if(it.get<intermediate::Nop>() &&
                it.get<const intermediate::Nop>()->type == intermediate::DelayType::WAIT_SFU)
            {
                it.erase();
                --numDelays;
            }
            else
                it.nextInBlock();
        }

        if(it.isEndOfBlock())
            throw CompilationError(CompilationStep::OPTIMIZER, "Failed to find both NOPs for waiting for SFU result");

        while(!it.isEndOfBlock())
        {
            if(it->readsRegister(REG_SFU_OUT))
            {
                it.reset((new intermediate::MoveOperation(it->getOutput().value(), result.value()))
                             ->copyExtrasFrom(it.get()));
                break;
            }
            else
                it.nextInBlock();
        }

        if(it.isEndOfBlock())
            throw CompilationError(CompilationStep::OPTIMIZER, "Failed to find the reading of the SFU result");

        // to not skip optimizing the resulting instruction
        it.previousInBlock();
    }
    return it;
}
