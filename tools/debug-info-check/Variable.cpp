#include "Variable.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"

using namespace llvm;

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
