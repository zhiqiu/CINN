#include "cinn/backends/llvm/llvm_util.h"

#include <glog/logging.h>
#include <llvm/Support/Alignment.h>

#include <atomic>
#include <mutex>  //NOLINT

namespace cinn {
namespace backends {

llvm::Type *CinnTypeToIrType(common::Type type, llvm::Module *m) {
  llvm::Type *ir_type = nullptr;
  if (type.is_cpp_const()) {
    // TODO(fc500110) support it latter.
  }

  llvm::Type *v   = llvm::Type::getVoidTy(m->getContext());
  llvm::Type *i8  = llvm::Type::getInt8Ty(m->getContext());
  llvm::Type *i32 = llvm::Type::getInt32Ty(m->getContext());
  llvm::Type *i64 = llvm::Type::getInt64Ty(m->getContext());
  llvm::Type *u32 = llvm::Type::getInt32Ty(m->getContext());
  llvm::Type *f32 = llvm::Type::getFloatTy(m->getContext());
  llvm::Type *f64 = llvm::Type::getDoubleTy(m->getContext());

  if (type.is_int(8)) {
    ir_type = i8;
  } else if (type.is_int(32)) {
    ir_type = i32;
  } else if (type.is_int(64)) {
    ir_type = i64;
  } else if (type.is_bool()) {
    ir_type = i8;
  } else if (type.is_float(32)) {
    ir_type = f32;
  } else if (type.is_float(64)) {
    ir_type = f64;
  } else if (type.is_void()) {
    ir_type = v;
  } else if (type.is_customized_type()) {
    CHECK(!type.customized_type().empty());
    ir_type = m->getTypeByName("struct." + type.customized_type());
  }
  CHECK(ir_type) << "LLVM can't convert type: " << type;

  // C array.
  if (type.lanes() > 1) {
    ir_type = llvm::ArrayType::get(ir_type, type.lanes());
  }

  if (type.is_cpp_handle()) {
    ir_type = llvm::PointerType::getUnqual(ir_type);
  }

  if (type.is_cpp_handle_handle()) {
    ir_type = llvm::PointerType::getUnqual(ir_type);
    ir_type = llvm::PointerType::getUnqual(ir_type);
  }

  return ir_type;
}

#define __(ty__)                                         \
  template <>                                            \
  llvm::Type *llvm_type_of<ty__>(llvm::Module * m) {     \
    return CinnTypeToIrType(common::type_of<ty__>(), m); \
  }

__(int32_t)
__(int64_t)
__(float)
__(double)
__(cinn_buffer_t)
__(cinn_buffer_t *)
__(cinn_pod_value_t *)
__(cinn_pod_value_t)

#undef __

}  // namespace backends
}  // namespace cinn
