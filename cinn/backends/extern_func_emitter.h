/**
 * \file Implements the ExternFuncEmitter class, which is the base of all the emitter of extern function in the
 * backends.
 */

#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "cinn/backends/extern_func_protos.h"
#include "cinn/ir/ir.h"

namespace cinn {
namespace backends {
class ExternFuncID;
}  // namespace backends
}  // namespace cinn

namespace std {
template <>
struct hash<cinn::backends::ExternFuncID> {
  size_t operator()(const cinn::backends::ExternFuncID& x) const;
};
}  // namespace std

namespace cinn {
namespace backends {

//! IDs of backends.
static const char* backend_C         = "C";
static const char* backend_llvm_host = "llvm_host";
static const char* backend_llvm_x86  = "llvm_x86";

/**
 * \brief Base class of the emitter of all the extern functions able to trigger inside CINN CodeGen system.
 * There are some common attributes and interfaces.
 */
class ExternFunctionEmitter {
 public:
  ExternFunctionEmitter() = default;

  virtual void BindCodeGen(void* codegen) = 0;
  /**
   * Get the name of the function.
   */
  virtual const char* func_name() const = 0;
  /**
   * Emit a store node, if the call node's RetValuePacked is true, otherwise Emit a Call node.
   */

  void Emit(const ir::Call* op) {
    func_proto().AssertMatch(op);
    EmitImpl(op);
  }

  const FunctionProto& func_proto() const;

  /**
   * \brief Tell whether the return value is packed to the argument list.
   *
   * e.g. Given the original IR
   * \code
   * s = Call(some_func, arg0)
   * \endcode
   *
   * If this function returns true, some pass will applied and transform the IR to
   * \code
   * Call(some_func, get_addr(s)
   * \endcode
   *
   * The `RetValuePacked` should be true when the external function modify an existing buffer (or some view of it) due
   * to that the C language can't return a container.
   */
  virtual bool RetValuePacked() const = 0;

  /**
   * @return the backend identifier of this emitter.
   */
  virtual const char* backend_kind() const = 0;

 protected:
  virtual void EmitImpl(const ir::Call* op) = 0;
};

struct ExternFuncID {
  std::string name;
  std::string backend_id;

  ExternFuncID(const char* name, const char* backend_id) : name(name), backend_id(backend_id) {}

  friend std::ostream& operator<<(std::ostream& os, const ExternFuncID& x);
  friend bool operator==(const ExternFuncID& a, const ExternFuncID& b) {
    return a.name == b.name && a.backend_id == b.backend_id;
  }
};

class ExternFunctionEmitterRegistry {
 public:
  static ExternFunctionEmitterRegistry& Global();

  void Register(const ExternFuncID& name, ExternFunctionEmitter* x);

  ExternFunctionEmitter* Lookup(const ExternFuncID& name) const;

 private:
  std::unordered_map<ExternFuncID, std::unique_ptr<ExternFunctionEmitter>> data_;

  ExternFunctionEmitterRegistry();
  CINN_DISALLOW_COPY_AND_ASSIGN(ExternFunctionEmitterRegistry);
};

}  // namespace backends
}  // namespace cinn
