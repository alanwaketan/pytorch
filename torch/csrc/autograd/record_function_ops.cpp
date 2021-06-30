#include <ATen/record_function.h>
#include <ATen/ThreadLocalState.h>

#include <torch/csrc/autograd/record_function_ops.h>
#include <torch/csrc/jit/runtime/custom_operator.h>

namespace caffe2 {
// Required for cpp_custom_type_hack to work
// NOLINTNEXTLINE(bugprone-exception-escape)
CAFFE_KNOWN_TYPE(at::RecordFunction);
} // namespace caffe2

namespace torch {
namespace autograd {
namespace profiler {

void RecordFunctionHolder::enter(const std::string& name) {
  record_function_ = std::make_unique<at::RecordFunction>(
    at::RecordScope::USER_SCOPE);
  record_function_->before(name);
}

void RecordFunctionHolder::exit() {
  TORCH_CHECK(record_function_ != nullptr,
              "record_function_ must be set via enter!");
  record_function_->end();
}

// Creates a new profiling scope using RecordFunction and invokes its starting
// callbacks.
c10::intrusive_ptr<RecordFunctionHolder> record_function_enter(
  const std::string& name) {
  auto wrapper = c10::make_intrusive<RecordFunctionHolder>();
  wrapper->enter(name);
  return wrapper;
}

// Ends the profiling scope created with record_function_enter.
void record_function_exit(c10::intrusive_ptr<RecordFunctionHolder> holder) {
  holder->exit();
}

c10::intrusive_ptr<c10::ivalue::Future> _call_end_callbacks_on_fut(
    c10::intrusive_ptr<RecordFunctionHolder> holder,
    const c10::intrusive_ptr<c10::ivalue::Future>& fut) {
  // Profiling callback that ends the associated record_function
  // and returns the value of the passed in future.
  std::function<c10::IValue(c10::ivalue::Future&)> futureProfilingFunc =
      [holder](c10::ivalue::Future& fut) {
        TORCH_INTERNAL_ASSERT(
            holder.defined(),
            "Undefined RecordFunction holder. This can happen if the handle is "
            "not correctly persisted and is destroyed before the future is "
            "realized.");
        holder->exit();
        // Note: this future is returned to the user to ensure that a call to wait()
        // ensures that profiling callbacks have ran. To ensure that this is
        // transparent, we must make this future propagate the value of the RPC
        // future.
        // Use value() here instead of constValue() to ensure we propagate errors.
        return fut.value();
      };
  // Define a future that completes after the profiling callbacks are run.
  auto profiledFut = fut->then(at::wrapPropagateTLSState(
      futureProfilingFunc),
      fut->elementType()
      );
  return profiledFut;
}

// Internal only, ensure Python understands this class. do not use directly.
TORCH_LIBRARY(profiler, m) {
  m.class_<RecordFunctionHolder>("_RecordFunctionHolder")
    .def(torch::init())
  ;
}

// Internal only, do not use directly, use Python's record_function()
TORCH_LIBRARY_FRAGMENT(profiler, m) {
    m.def(
      "_record_function_enter(str x) -> __torch__.torch.classes.profiler._RecordFunctionHolder Y",
      record_function_enter
    );
    m.def(
      "_record_function_exit(__torch__.torch.classes.profiler._RecordFunctionHolder x) -> ()",
      record_function_exit
    );
}

// Needed to register JIT operator in operator registry below
c10::AliasAnalysisKind aliasAnalysisFromSchema() {
  return c10::AliasAnalysisKind::FROM_SCHEMA;
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
jit::RegisterOperators reg_fut_ops({
    jit::Operator(
        "profiler::_call_end_callbacks_on_jit_fut(__torch__.torch.classes.profiler._RecordFunctionHolder x, Future(t) y) -> Future(t)",
        [](jit::Stack* stack) {
          // Pop inputs, which should be a future and a tensor
          auto fut = jit::pop(stack).toFuture();
          auto record_function_holder = jit::pop(stack).toCustomClass<RecordFunctionHolder>();
          auto profiledFut = _call_end_callbacks_on_fut(record_function_holder, fut);
          // return future that completes when profiling callbacks have run.
          jit::push(stack, std::move(profiledFut));
        },
        aliasAnalysisFromSchema()),
});

} // namespace profiler
} // namespace autograd
} // namespace torch
