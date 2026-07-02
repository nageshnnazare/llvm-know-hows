; test.ll — input for the mul-to-add pass.
; `opt -passes=mul-to-add` should turn `mul %x, 2` into `add %x, %x`.
define i32 @double_it(i32 %x) {
entry:
  %t = mul i32 %x, 2
  ret i32 %t
}

define i32 @triple_it(i32 %x) {
entry:
  %t = mul i32 %x, 3      ; NOT rewritten (constant is 3, not 2)
  ret i32 %t
}
