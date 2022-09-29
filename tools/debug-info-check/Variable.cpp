#include "Variable.h"

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
  if (producer == other)
    return true;

  // As mentioned in `replaceAllDbgUsesWith`, integers wider than the source
  // variable are assumed to be safe without needing explicit conversions.
  if (const auto *producerInt = dyn_cast<ConstantInt>(producer)) {
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

ref<Expr> Assignment::evaluate() {
  if (evaluatedSymbolicValue)
    return evaluatedSymbolicValue;

  // Empty expression
  const auto *expr = varIntrinsic->getExpression();
  if (!expr->getNumElements()) {
    evaluatedSymbolicValue = producedSymbolicValue;
    return evaluatedSymbolicValue;
  }

  assert(expr->isValid() && "Invalid dbg intrinsic expression");
  // TODO: Support other intrinsics with expressions
  assert(isa<DbgValueInst>(varIntrinsic) &&
         "Unexpected dbg intrinsic with expression");

  SmallVector<ref<Expr>> stack;
  // TODO: Handle DIArgList case
  stack.push_back(producedSymbolicValue);
  KLEE_DEBUG(dbgs() << "Pushed initial value onto stack: "
                    << producedSymbolicValue << "\n");
  ExprBuilder *builder = createDefaultExprBuilder();

  // Only value expressions are supported, so all non-empty expressions should
  // be terminated with the stack value operation.
  bool isValueExpr = false;

  for (const auto &exprOp : expr->expr_ops()) {
    const auto &opcode = exprOp.getOp();
    switch (opcode) {
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
    // 0x9f / 159
    // Specifies that the object does not exist in memory but its value is
    // nonetheless known and is at the top of the stack.
    case dwarf::DW_OP_stack_value: {
      isValueExpr = true;
    } break;
    default: {
      KLEE_DEBUG(dbgs() << "Current opcode: " << opcode << "\n");
      llvm_unreachable("Unexpected expression opcode");
    } break;
    }
  }

  // Apply same implicit truncation debuggers use if source variable is smaller
  // width than the current value.
  const auto *variable = varIntrinsic->getVariable();
  const auto &varWidth = variable->getSizeInBits().getValue();
  assert(stack.back()->getWidth() >= varWidth &&
         "Expression result smaller than variable width");
  if (varWidth < stack.back()->getWidth()) {
    stack.back() = builder->Extract(stack.back(), 0, varWidth);
  }

  evaluatedSymbolicValue = stack.back();
  KLEE_DEBUG(dbgs() << "Result: " << evaluatedSymbolicValue << "\n");

  assert(stack.size() == 1 && "Expression stack has unexpected size");
  assert(isValueExpr && "Unexpected non-value expression");

  return evaluatedSymbolicValue;
}
