#include "Variable.h"
#include "ValuesCollector.h"

#include "klee/ADT/Ref.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/ExprBuilder.h"
#include "klee/Support/Debug.h"

#include "llvm/ADT/APInt.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"

using namespace klee;
using namespace llvm;

#define DEBUG_TYPE "variable"

bool Assignment::isValueConsistent(const Variable &var,
                                   const Value *other) const {
  assert(producers.size() == 1 &&
         "isValueConsistent is unimplemented for multiple producers");

  if (producers[0] == other)
    return true;

  // As mentioned in `replaceAllDbgUsesWith`, integers wider than the source
  // variable are assumed to be safe without needing explicit conversions.
  if (const auto *producerInt = dyn_cast<ConstantInt>(producers[0])) {
    if (const auto *otherInt = dyn_cast<ConstantInt>(other)) {
      const auto varSize = var.diVariable->getSizeInBits().getValue();
      assert(producerInt->getType()->getPrimitiveSizeInBits() >= varSize &&
             "Producer width smaller than source variable width");
      assert(otherInt->getType()->getPrimitiveSizeInBits() >= varSize &&
             "Other width smaller than source variable width");
      return APInt::isSameValue(producerInt->getValue(), otherInt->getValue());
    }
  }

  // TODO: Handle bit cast case...?

  return false;
}

void autoTruncate(ExprBuilder *builder, ref<Expr> &arg1, ref<Expr> &arg2) {
  const auto width1 = arg1->getWidth();
  const auto width2 = arg2->getWidth();
  if (width1 == width2)
    return;
  if (width1 < width2) {
    arg2 = builder->Extract(arg2, 0, width1);
    return;
  }
  if (width2 < width1) {
    arg1 = builder->Extract(arg1, 0, width2);
    return;
  }
  llvm_unreachable("Unexpected narrowing case");
}

void truncateToVariable(ExprBuilder *builder, const DILocalVariable *variable,
                        ref<Expr> &value) {
  // Apply same implicit truncation debuggers use if source variable is smaller
  // width than the current value.
  const auto &varWidth = variable->getSizeInBits().getValue();
  assert(value->getWidth() >= varWidth && "Value smaller than variable width");
  if (varWidth < value->getWidth()) {
    if (auto constantValue = dyn_cast<klee::ConstantExpr>(value)) {
      // Truncate constants directly
      value = constantValue->Extract(0, varWidth);
      return;
    }
    ref<Expr> width = klee::ConstantExpr::alloc(0, varWidth);
    autoTruncate(builder, value, width);
  }
}

ref<Expr> Assignment::evaluate() {
  if (evaluatedSymbolicValue)
    return evaluatedSymbolicValue;

  if (varIntrinsic->isUndef()) {
    KLEE_DEBUG(dbgs() << "Variable intrinsic with undef input"
                      << "\n");
    return nullptr;
  }

  if (producedSymbolicValues.size() != producers.size()) {
    KLEE_DEBUG(dbgs() << "Expected " << producers.size()
                      << " symbolic value(s), got "
                      << producedSymbolicValues.size() << "\n");
    return nullptr;
  }

  const auto *variable = varIntrinsic->getVariable();
  ExprBuilder *builder = createDefaultExprBuilder();

  // User is something other than `dbg.value` intrinsic
  // These are store-like cases where there's no value expression to apply
  if (!isa<DbgValueInst>(user)) {
    assert(producers.size() == 1 && "Store-like user with multiple producers");
    assert(producedSymbolicValues.size() == 1 &&
           "Symbolic value missing for producer");
    evaluatedSymbolicValue = producedSymbolicValues[0];
    truncateToVariable(builder, variable, evaluatedSymbolicValue);
    return evaluatedSymbolicValue;
  }

  // Empty expression
  const auto *expr = varIntrinsic->getExpression();
  if (!expr->getNumElements()) {
    assert(producers.size() == 1 &&
           "Empty dbg intrinsic expression with multiple inputs");
    assert(producedSymbolicValues.size() == 1 &&
           "Symbolic value missing for producer");
    evaluatedSymbolicValue = producedSymbolicValues[0];
    truncateToVariable(builder, variable, evaluatedSymbolicValue);
    return evaluatedSymbolicValue;
  }

  assert(expr->isValid() && "Invalid dbg intrinsic expression");
  // TODO: Support other intrinsics with expressions
  assert(isa<DbgValueInst>(varIntrinsic) &&
         "Unexpected dbg intrinsic with expression");

  SmallVector<ref<Expr>> stack;
  // If there's a single input, then that is automatically placed on the stack
  // by default. Otherwise, we expect explicit ops to push each value onto the
  // stack manually.
  if (producers.size() == 1) {
    assert(producedSymbolicValues.size() == 1 &&
           "Symbolic value missing for producer");
    stack.push_back(producedSymbolicValues[0]);
    KLEE_DEBUG(dbgs() << "Pushed initial value onto stack: "
                      << producedSymbolicValues[0] << "\n");
  }

  // Only value expressions are supported, so all non-empty expressions should
  // be terminated with the stack value operation.
  // The deref operation is also accepted and treated as a marker to capture the
  // current value stored at an address.
  bool isValidExpr = false;

  for (const auto &exprOp : expr->expr_ops()) {
    const auto &opcode = exprOp.getOp();

    // Reset validity for each new operation
    isValidExpr = false;

    switch (opcode) {
    // 0x06 / 006
    // Pops the top stack entry and treats it as an address. The value retrieved
    // from that address is pushed.
    case dwarf::DW_OP_deref: {
      auto arg = stack.back();
      const auto result = arg->deref();
      KLEE_DEBUG(dbgs() << "deref: " << result << "\n");
      stack.back() = std::move(result);
      isValidExpr = true;
    } break;
    // 0x10 / 016
    // Provides an unsigned integer constant.
    case dwarf::DW_OP_constu:
    // 0x11 / 017
    // Provides a signed integer constant.
    case dwarf::DW_OP_consts: {
      const auto &arg = exprOp.getArg(0);
      // Machine word size used as a generic width for constants
      const auto result = builder->Constant(arg, Expr::Int64);
      KLEE_DEBUG(dbgs() << "constu/s: " << result << "\n");
      stack.push_back(std::move(result));
    } break;
    // 0x1a / 026
    // Pops the top two stack entries, performs a bitwise and operation on the
    // two, and pushes the result.
    case dwarf::DW_OP_and: {
      auto arg1 = stack.back();
      stack.pop_back();
      auto &arg2 = stack.back();
      autoTruncate(builder, arg1, arg2);
      const auto result = builder->And(arg1, arg2);
      KLEE_DEBUG(dbgs() << "and: " << result << "\n");
      stack.back() = std::move(result);
    } break;
    // 0x1b / 027
    // Pops the top two stack values, divides the former second entry by the
    // former top of the stack using signed division, and pushes the result.
    case dwarf::DW_OP_div: {
      auto arg1 = stack.back();
      stack.pop_back();
      auto &arg2 = stack.back();
      autoTruncate(builder, arg1, arg2);
      const auto result = builder->SDiv(arg2, arg1);
      KLEE_DEBUG(dbgs() << "div: " << result << "\n");
      stack.back() = std::move(result);
    } break;
    // 0x1c / 028
    // Pops the top two stack values, subtracts the 2 former top of the stack
    // from the former second entry, and pushes the result.
    case dwarf::DW_OP_minus: {
      auto arg1 = stack.back();
      stack.pop_back();
      auto &arg2 = stack.back();
      autoTruncate(builder, arg1, arg2);
      const auto result = builder->Sub(arg2, arg1);
      KLEE_DEBUG(dbgs() << "minus: " << result << "\n");
      stack.back() = std::move(result);
    } break;
    // 0x1d / 029
    // Pops the top two stack values, calculates the former second entry modulo
    // the former top of the stack, and pushes the result.
    case dwarf::DW_OP_mod: {
      auto arg1 = stack.back();
      stack.pop_back();
      auto &arg2 = stack.back();
      autoTruncate(builder, arg1, arg2);
      const auto result = builder->SRem(arg2, arg1);
      KLEE_DEBUG(dbgs() << "mod: " << result << "\n");
      stack.back() = std::move(result);
    } break;
    // 0x1e / 030
    // Pops the top two stack entries, multiplies them together, and pushes the
    // result.
    case dwarf::DW_OP_mul: {
      auto arg1 = stack.back();
      stack.pop_back();
      auto &arg2 = stack.back();
      autoTruncate(builder, arg1, arg2);
      const auto result = builder->Mul(arg1, arg2);
      KLEE_DEBUG(dbgs() << "mul: " << result << "\n");
      stack.back() = std::move(result);
    } break;
    // 0x21 / 033
    // Pops the top two stack entries, performs a bitwise or operation on the
    // two, and pushes the result.
    case dwarf::DW_OP_or: {
      auto arg1 = stack.back();
      stack.pop_back();
      auto &arg2 = stack.back();
      autoTruncate(builder, arg1, arg2);
      const auto result = builder->Or(arg1, arg2);
      KLEE_DEBUG(dbgs() << "or: " << result << "\n");
      stack.back() = std::move(result);
    } break;
    // 0x22 / 034
    // Pops the top two stack entries, adds them together, and pushes the
    // result.
    case dwarf::DW_OP_plus: {
      auto arg1 = stack.back();
      stack.pop_back();
      auto &arg2 = stack.back();
      autoTruncate(builder, arg1, arg2);
      const auto result = builder->Add(arg1, arg2);
      KLEE_DEBUG(dbgs() << "plus: " << result << "\n");
      stack.back() = std::move(result);
    } break;
    // 0x23 / 035
    // Pops the top stack entry, adds it to the unsigned constant operand, and
    // pushes the result.
    case dwarf::DW_OP_plus_uconst: {
      auto arg1 = stack.back();
      // Machine word size used as a generic width for constants
      auto arg2 = builder->Constant(exprOp.getArg(0), Expr::Int64);
      autoTruncate(builder, arg1, arg2);
      const auto result = builder->Add(arg1, arg2);
      KLEE_DEBUG(dbgs() << "plus_uconst: " << result << "\n");
      stack.back() = std::move(result);
    } break;
    // 0x24 / 036
    // Pops the top two stack entries, shifts the former second entry left
    // (filling with zero bits) by the number of bits specified by the former
    // top of the stack, and pushes the result.
    case dwarf::DW_OP_shl: {
      auto arg1 = stack.back();
      stack.pop_back();
      auto &arg2 = stack.back();
      autoTruncate(builder, arg1, arg2);
      const auto result = builder->Shl(arg2, arg1);
      KLEE_DEBUG(dbgs() << "shl: " << result << "\n");
      stack.back() = std::move(result);
    } break;
    // 0x25 / 037
    // Pops the top two stack entries, shifts the former second entry right
    // logically (filling with zero bits) by the number of bits specified by the
    // former top of the stack, and pushes the result.
    case dwarf::DW_OP_shr: {
      auto arg1 = stack.back();
      stack.pop_back();
      auto &arg2 = stack.back();
      autoTruncate(builder, arg1, arg2);
      const auto result = builder->LShr(arg2, arg1);
      KLEE_DEBUG(dbgs() << "shr: " << result << "\n");
      stack.back() = std::move(result);
    } break;
    // 0x26 / 038
    // Pops the top two stack entries, shifts the former second entry right
    // arithmetically (divide the magnitude by 2, keep the same sign for the
    // result) by the number of bits specified by the former top of the stack,
    // and pushes the result.
    case dwarf::DW_OP_shra: {
      auto arg1 = stack.back();
      stack.pop_back();
      auto &arg2 = stack.back();
      autoTruncate(builder, arg1, arg2);
      const auto result = builder->AShr(arg2, arg1);
      KLEE_DEBUG(dbgs() << "shra: " << result << "\n");
      stack.back() = std::move(result);
    } break;
    // 0x27 / 039
    // Pops the top two stack entries, performs a bitwise exclusive-or operation
    // on the two, and pushes the result.
    case dwarf::DW_OP_xor: {
      auto arg1 = stack.back();
      stack.pop_back();
      auto &arg2 = stack.back();
      autoTruncate(builder, arg1, arg2);
      const auto result = builder->Xor(arg1, arg2);
      KLEE_DEBUG(dbgs() << "xor: " << result << "\n");
      stack.back() = std::move(result);
    } break;
    // 0x9f / 159
    // Specifies that the object does not exist in memory but its value is
    // nonetheless known and is at the top of the stack.
    case dwarf::DW_OP_stack_value: {
      isValidExpr = true;
    } break;
    // 0x1001 / 4097
    // Converts the bit width of the top value on the stack. Takes additional
    // arguments specifying the output width in bits and signedness to use for
    // the conversion.
    case dwarf::DW_OP_LLVM_convert: {
      const auto &bits = exprOp.getArg(0);
      const auto &signednessRaw = exprOp.getArg(1);
      assert((signednessRaw == dwarf::DW_ATE_signed ||
              signednessRaw == dwarf::DW_ATE_unsigned) &&
             "Unexpected signedness value");
      const bool signedness = signednessRaw == dwarf::DW_ATE_signed;
      ref<Expr> result;
      if (bits == stack.back()->getWidth()) {
        KLEE_DEBUG(dbgs() << "convert: already " << bits
                          << " bit(s), skipping\n");
        continue;
      }
      if (bits < stack.back()->getWidth())
        result = builder->Extract(stack.back(), 0, bits);
      else if (signedness)
        result = builder->SExt(stack.back(), bits);
      else
        result = builder->ZExt(stack.back(), bits);
      KLEE_DEBUG(dbgs() << "convert: " << result << "\n");
      stack.back() = std::move(result);
    } break;
    // 0x1005 / 4101
    // Pushes one of several input values onto the stack identified by an index
    // argument.
    case dwarf::DW_OP_LLVM_arg: {
      assert(producers.size() > 1 &&
             "Argument opcode not expected with a single input");
      const auto &index = exprOp.getArg(0);
      assert(producedSymbolicValues.size() >= index + 1 &&
             "Symbolic value missing for producer");
      const auto result = producedSymbolicValues[index];
      KLEE_DEBUG(dbgs() << "LLVM_arg: " << result << "\n");
      stack.push_back(std::move(result));
    } break;
    default: {
      KLEE_DEBUG(dbgs() << "Current opcode: " << opcode << "\n");
      llvm_unreachable("Unexpected expression opcode");
    } break;
    }
  }

  evaluatedSymbolicValue = stack.back();
  truncateToVariable(builder, variable, evaluatedSymbolicValue);

  KLEE_DEBUG(dbgs() << "Result: " << evaluatedSymbolicValue << "\n");

  assert(stack.size() == 1 && "Expression stack has unexpected size");
  assert(isValidExpr && "Invalid or unexpected expression");

  delete builder;

  return evaluatedSymbolicValue;
}
