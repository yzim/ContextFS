;; Simple Wasm module that returns SoftWatch (1) for Write ops (1), Allow (0) otherwise
(module
  (func $on_event (export "on_event") (param $op i32) (result i32)
    (if (i32.eq (local.get $op) (i32.const 1))
      (then (return (i32.const 1)))
    )
    (i32.const 0)
  )
)
