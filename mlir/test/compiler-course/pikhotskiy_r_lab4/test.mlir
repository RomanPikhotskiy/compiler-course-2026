// RUN: mlir-opt -load-pass-plugin=%mlir_lib_dir/pikhotskiy_r_lab4_MLIR%shlibext -split-input-file --pass-pipeline="builtin.module(pikhotskiy-call-count)" %s | FileCheck %s
// RUN: mlir-opt -load-pass-plugin=%mlir_lib_dir/pikhotskiy_r_lab4_MLIR%shlibext -split-input-file --pass-pipeline="builtin.module(pikhotskiy-call-count,pikhotskiy-call-count)" %s | FileCheck %s

module {
  // CHECK-LABEL: func.func @entry
  // CHECK-SAME: call_count = 0 : i64
  func.func @entry() {
    func.call @worker() : () -> ()
    func.call @worker() : () -> ()
    func.call @recursive() : () -> ()
    func.call @declared_only() : () -> ()
    func.return
  }

  // CHECK-LABEL: func.func @worker
  // CHECK-SAME: call_count = 2 : i64
  func.func @worker() {
    func.return
  }

  // CHECK-LABEL: func.func @recursive
  // CHECK-SAME: call_count = 1 : i64
  func.func @recursive() {
    func.call @recursive() : () -> ()
    func.return
  }

  // CHECK-LABEL: func.func private @declared_only
  // CHECK-SAME: call_count = 1 : i64
  func.func private @declared_only()
}

// -----

// Count call sites from multiple callers, replace stale counts, and preserve
// unrelated attributes. The callee appears before both callers.
module {
  // CHECK-LABEL: func.func @shared_result
  // CHECK-SAME: call_count = 3 : i64
  // CHECK-SAME: note = "preserve"
  func.func @shared_result(%value: i32) -> i32 attributes {call_count = 91 : i64, note = "preserve"} {
    func.return %value : i32
  }

  // CHECK-LABEL: func.func @first_user
  // CHECK-SAME: call_count = 0 : i64
  func.func @first_user(%value: i32) -> i32 {
    %a = func.call @shared_result(%value) : (i32) -> i32
    %b = func.call @shared_result(%a) : (i32) -> i32
    func.return %b : i32
  }

  // CHECK-LABEL: func.func @second_user
  // CHECK-SAME: call_count = 0 : i64
  func.func @second_user(%value: i32) -> i32 {
    %a = func.call @shared_result(%value) : (i32) -> i32
    func.return %a : i32
  }

  // CHECK-LABEL: func.func private @unused_declaration
  // CHECK-SAME: call_count = 0 : i64
  func.func private @unused_declaration() attributes {call_count = 17 : i64}
}

// -----

// Ignore direct self-calls, but count calls between two different functions.
module {
  // CHECK-LABEL: func.func @self_only
  // CHECK-SAME: call_count = 0 : i64
  func.func @self_only() {
    func.call @self_only() : () -> ()
    func.return
  }

  // CHECK-LABEL: func.func @cycle_start
  // CHECK-SAME: call_count = 1 : i64
  func.func @cycle_start() {
    func.call @cycle_finish() : () -> ()
    func.return
  }

  // CHECK-LABEL: func.func @cycle_finish
  // CHECK-SAME: call_count = 1 : i64
  func.func @cycle_finish() {
    func.call @cycle_start() : () -> ()
    func.return
  }
}

// -----

// A loop contains one call operation, regardless of its runtime trip count.
module {
  // CHECK-LABEL: func.func @loop_user
  // CHECK-SAME: call_count = 0 : i64
  func.func @loop_user() {
    %begin = arith.constant 0 : index
    %end = arith.constant 6 : index
    %step = arith.constant 1 : index
    scf.for %i = %begin to %end step %step {
      func.call @loop_target() : () -> ()
    }
    func.return
  }

  // CHECK-LABEL: func.func @loop_target
  // CHECK-SAME: call_count = 1 : i64
  func.func @loop_target() {
    func.return
  }
}

// -----

// Identical names in different symbol tables refer to different functions.
module {
  // CHECK-LABEL: func.func @scoped_target
  // CHECK-SAME: call_count = 0 : i64
  func.func @scoped_target() {
    func.return
  }

  // CHECK-LABEL: module @north
  module @north {
    // CHECK-LABEL: func.func @scoped_target
    // CHECK-SAME: call_count = 1 : i64
    func.func @scoped_target() {
      func.call @scoped_target() : () -> ()
      func.return
    }
    // CHECK-LABEL: func.func @scoped_user
    // CHECK-SAME: call_count = 0 : i64
    func.func @scoped_user() {
      func.call @scoped_target() : () -> ()
      func.return
    }
  }

  // CHECK-LABEL: module @south
  module @south {
    // CHECK-LABEL: func.func @scoped_target
    // CHECK-SAME: call_count = 2 : i64
    func.func @scoped_target() {
      func.return
    }
    // CHECK-LABEL: func.func @scoped_user
    // CHECK-SAME: call_count = 0 : i64
    func.func @scoped_user() {
      func.call @scoped_target() : () -> ()
      func.call @scoped_target() : () -> ()
      func.return
    }
  }
}

// -----

// CHECK-LABEL: module @empty
// CHECK-NEXT: }
module @empty {}
