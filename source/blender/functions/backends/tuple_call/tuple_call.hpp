#pragma once

/**
 * The tuple-call calling convention is the main type of function bodies for the pure C++ backend
 * (without JIT compilation). A function implementing the tuple-call body takes a tuple as input
 * and outputs a tuple containing the computed values.
 */

#include "FN_cpp.hpp"
#include "execution_context.hpp"

namespace FN {

class TupleCallBodyBase : public FunctionBody {
 private:
  TupleMeta m_meta_in;
  TupleMeta m_meta_out;

 protected:
  void owner_init_post() override;

 public:
  virtual ~TupleCallBodyBase(){};

  virtual void init_defaults(Tuple &fn_in) const;

  /**
   * Get the metadata for tuples that this function can take as input.
   */
  TupleMeta &meta_in()
  {
    return m_meta_in;
  }

  /**
   * Get the metadata for tuples that this function can output.
   */
  TupleMeta &meta_out()
  {
    return m_meta_out;
  }
};

class TupleCallBody : public TupleCallBodyBase {
 public:
  static const uint FUNCTION_BODY_ID = 1;
  using FunctionBodyType = TupleCallBody;

  /**
   * Calls the function with additional stack frames.
   */
  inline void call__setup_stack(Tuple &fn_in, Tuple &fn_out, ExecutionContext &ctx) const
  {
    TextStackFrame frame(this->owner().name().data());
    ctx.stack().push(&frame);
    this->call(fn_in, fn_out, ctx);
    ctx.stack().pop();
  }

  inline void call__setup_stack(Tuple &fn_in,
                                Tuple &fn_out,
                                ExecutionContext &ctx,
                                StackFrame &extra_frame) const
  {
    ctx.stack().push(&extra_frame);
    this->call__setup_stack(fn_in, fn_out, ctx);
    ctx.stack().pop();
  }

  inline void call__setup_stack(Tuple &fn_in,
                                Tuple &fn_out,
                                ExecutionContext &ctx,
                                SourceInfo *source_info) const
  {
    SourceInfoStackFrame frame(source_info);
    this->call__setup_stack(fn_in, fn_out, ctx, frame);
  }

  inline void call__setup_execution_context(Tuple &fn_in, Tuple &fn_out) const
  {
    ExecutionStack stack;
    ExecutionContext ctx(stack);
    this->call__setup_stack(fn_in, fn_out, ctx);
  }

  /**
   * This function has to be implemented for every tuple-call body. It takes in two references to
   * different tuples and the current execution context.
   *
   * By convention, when the function is called, the ownership of the data in both tuples is this
   * function. That means, that values from fn_in can also be destroyed or relocated if
   * appropriate. If fn_in still contains initialized values when this function ends, they will be
   * destructed.
   *
   * The output tuple fn_out can already contain data beforehand, but can also contain only
   * uninitialized data. When this function ends, it is expected that every element in fn_out is
   * initialized.
   */
  virtual void call(Tuple &fn_in, Tuple &fn_out, ExecutionContext &ctx) const = 0;
};

class LazyState {
 private:
  uint m_entry_count = 0;
  bool m_is_done = false;
  void *m_user_data;
  Vector<uint> m_requested_inputs;

 public:
  LazyState(void *user_data) : m_user_data(user_data)
  {
  }

  void start_next_entry()
  {
    m_entry_count++;
    m_requested_inputs.clear();
  }

  void request_input(uint index)
  {
    m_requested_inputs.append(index);
  }

  void done()
  {
    m_is_done = true;
  }

  const Vector<uint> &requested_inputs() const
  {
    return m_requested_inputs;
  }

  bool is_first_entry() const
  {
    return m_entry_count == 1;
  }

  bool is_done() const
  {
    return m_is_done;
  }

  void *user_data() const
  {
    return m_user_data;
  }
};

/**
 * Similar to the normal tuple-call body, but supports lazy input evaluation. That means, that not
 * all its input have to be computed before it is executed. The call function can request which
 * inputs it needs by e.g. first checking other elements in fn_in.
 *
 * To avoid recomputing the same temporary data multiple times, the function can get a memory
 * buffer of a custom size to store custom data until it is done.
 */
class LazyInTupleCallBody : public TupleCallBodyBase {
 public:
  static const uint FUNCTION_BODY_ID = 2;
  using FunctionBodyType = LazyInTupleCallBody;

  /**
   * Required buffer size for temporary data.
   */
  virtual uint user_data_size() const;

  /**
   * Indices of function inputs that are required in any case. Those elements can be expected to be
   * initialized when call is called for the first time.
   */
  virtual const Vector<uint> &always_required() const;

  /**
   * The ownership semantics are the same as in the normal tuple-call. The only difference is the
   * additional LazyState parameter. With it, other inputs can be requested or the execution of the
   * function can be marked as done.
   */
  virtual void call(Tuple &fn_in,
                    Tuple &fn_out,
                    ExecutionContext &ctx,
                    LazyState &state) const = 0;

  inline void call__setup_stack(Tuple &fn_in,
                                Tuple &fn_out,
                                ExecutionContext &ctx,
                                LazyState &state) const
  {
    TextStackFrame frame(this->owner().name().data());
    ctx.stack().push(&frame);
    this->call(fn_in, fn_out, ctx, state);
    ctx.stack().pop();
  }

  inline void call__setup_stack(Tuple &fn_in,
                                Tuple &fn_out,
                                ExecutionContext &ctx,
                                LazyState &state,
                                StackFrame &extra_frame) const
  {
    ctx.stack().push(&extra_frame);
    this->call__setup_stack(fn_in, fn_out, ctx, state);
    ctx.stack().pop();
  }

  inline void call__setup_stack(Tuple &fn_in,
                                Tuple &fn_out,
                                ExecutionContext &ctx,
                                LazyState &state,
                                SourceInfo *source_info) const
  {
    SourceInfoStackFrame frame(source_info);
    this->call__setup_stack(fn_in, fn_out, ctx, state, frame);
  }
};

class FunctionInputNamesProvider final : public TupleElementNameProvider {
 private:
  Function &m_function;

 public:
  FunctionInputNamesProvider(Function &function) : m_function(function)
  {
  }

  StringRefNull get_element_name(uint index) const override
  {
    return m_function.input_name(index);
  }
};

class FunctionOutputNamesProvider final : public TupleElementNameProvider {
 private:
  Function &m_function;

 public:
  FunctionOutputNamesProvider(Function &function) : m_function(function)
  {
  }

  StringRefNull get_element_name(uint index) const override
  {
    return m_function.output_name(index);
  }
};

} /* namespace FN */

/**
 * Allocate input and output tuples for a particular tuple-call body.
 */
#define FN_TUPLE_CALL_ALLOC_TUPLES(body, name_in, name_out) \
  FN_TUPLE_STACK_ALLOC(name_in, (body).meta_in()); \
  FN_TUPLE_STACK_ALLOC(name_out, (body).meta_out())

#define FN_TUPLE_CALL_NAMED_REF(THIS, FN_IN, FN_OUT, R_INPUTS, R_OUTPUTS) \
  FN::FunctionInputNamesProvider _input_names(THIS->owner()); \
  FN::FunctionOutputNamesProvider _output_names(THIS->owner()); \
  FN::NamedTupleRef R_INPUTS(&FN_IN, &_input_names); \
  FN::NamedTupleRef R_OUTPUTS(&FN_OUT, &_output_names)
