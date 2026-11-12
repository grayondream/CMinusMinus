# CDD Closure Escape and First-Class Storage Design

## Intent

Remove two related limitations in the current closure implementation:

1. By-value capturing closures can escape by return, but by-reference and
   `this`-capturing closures are rejected in semantic analysis. The rejection
   makes the compiler decide a lifetime question that the language's raw-pointer
   model already leaves to the programmer.
2. Capturing closures are nominal anonymous structs, but the type cannot be
   named at source level. Consequently a closure type cannot be used for a
   struct field, array element, alias, or ordinary parameter, even though the
   generated value is an ordinary by-value struct.

The change keeps CDD's no-hidden-allocation model. It does not introduce heap
promotion, reference counting, GC, or type erasure.

## Naming and syntax

- `decltype(expr)` is a type specifier that resolves to the type of `expr`
  without evaluating it at runtime.
- `auto name[] = { ... }` / `auto name[N] = { ... }` infers the element type
  from the first initializer and checks the remaining elements for
  compatibility.
- Instance methods may use `auto` return type deduction. Static methods remain
  unsupported for `auto` return.
- Closure calls work through arbitrary closure-valued expressions:
  `obj.field(args)`, `arr[i](args)`, and `make_closure()(args)`.

Examples:

```cdd
auto make_adder(int32 x) {
    return [x](int32 y) -> int32 { return x + y; };
}

using Adder = decltype(make_adder(0));

struct Box {
    Adder f;
};

int32 main() {
    Box b = { make_adder(40) };
    auto a[2] = { make_adder(1), make_adder(2) };
    return b.f(2) + a[0](1) + a[1](1) - 48;
}
```

## Escape semantics

- Returning a by-value capturing closure continues to copy the closure struct.
- Returning a closure with by-reference captures or a `this` capture is now
  allowed and emits warning `W3006`. The closure copies its fields exactly like
  any other struct; it does not extend the lifetime of captured storage.
- The caller is responsible for ensuring captured locals, `this` objects, and
  other referenced storage outlive the closure. This matches the existing raw
  pointer model.
- A returned closure with a nested by-reference/`this` closure still propagates
  the warning through by-value capture fields.
- No new runtime allocation or cleanup is generated.

## Auto-return instance methods

- In-class and out-of-class instance methods may infer their return type from
  the first value return.
- The class method table is updated with the inferred return type so calls
  after the class definition see the concrete closure type.
- Static methods, `compile_time`, and `compile_only` functions still reject
  `auto` returns. Auto-return methods keep the existing source-order rule:
  the method body must be analyzed before a later call resolves.
- Method prototypes without a body still report `must have a body`.

## `decltype`

- `decltype(expr)` is resolved during semantic analysis in the scope where the
  type appears. It returns the analyzed expression type without code generation.
- It is usable anywhere a type-id is accepted: `using`, fields, arrays,
  parameters, locals, return types, `sizeof`, casts, and compound literals.
- The expression may be a variable, member access, call, or other typed
  expression. It is not evaluated.
- The resolved closure type remains nominal: two independent lambda literals
  still have distinct types. `decltype` makes one existing nominal type
  nameable; it does not merge structurally identical lambdas.
- A `decltype` cycle or an expression with no type is a semantic error.
- `decltype` in a template body is not a goal for this change; template
  instantiation of a `decltype` expression remains unsupported and will be
  diagnosed rather than silently misbound.

## Auto arrays

- For `auto arr[] = { e0, e1, ... }`, the element type is the type of `e0`
  after normal expression analysis. The length is the initializer count.
- For `auto arr[N] = { ... }`, the same element inference applies and the
  existing initializer-count checks remain.
- Every later element must be compatible with `e0`; incompatible elements are
  diagnosed with the existing assignment-compatibility machinery.
- A missing or non-brace initializer for an `auto` array is an error.
- This permits arrays of the same nominal closure type, including different
  captured values produced by the same lambda expression.

## Expression closure calls

- When an expression callee has a closure `StructType`, semantic analysis
  records the closure call signature and call-function symbol, just as it does
  for a named closure variable.
- Codegen takes the callee expression's address when it is an lvalue (field or
  array element). For an rvalue closure (for example a returned closure), it
  stores the value in a temporary alloca and passes that address.
- Named closure variables keep the existing `lookupVariableAddr` path.

## Non-goals

- Heterogeneous arrays or type-erased `Closure` values.
- Heap-promoted/by-ref-boxed captures, reference counting, or GC.
- Changing by-value capture copy semantics.
- C++-style structural compatibility between independent lambda literals.
- Capturing lambdas in file-scope initializers.
- Auto-return static methods, `compile_time`, or `compile_only` functions.

## Diagnostics

- New warning `W3006`: returned closure captures by-reference storage or
  `this`; captured storage must outlive the closure.
- `auto` array without a brace initializer: `auto array '<name>' requires a brace-enclosed initializer`.
- `auto` array with incompatible elements: the existing incompatible-assignment
  diagnostic, prefixed with the array element context.
- `decltype` of an untyped expression: `cannot deduce a type from decltype expression`.
- `decltype` cycle: `recursive decltype in type definition`.
- Static auto method: existing unsupported-method diagnostic narrowed to
  static methods.
- Method prototype without body: existing `must have a body` diagnostic.

## Acceptance

- Returning by-reference and `this` closures compiles and emits W3006; no hard
  semantic error.
- `this` closure can be returned from an auto-return instance method and called
  while the object is alive.
- `decltype(make(...))` names a closure type used in a struct field and an
  array; field/array closure calls execute correctly.
- `auto` arrays of closures work with both explicit and inferred length.
- Existing by-value closure escape, local by-reference closure calls, lambda
  capture diagnostics, and 1888-test baseline do not regress.
