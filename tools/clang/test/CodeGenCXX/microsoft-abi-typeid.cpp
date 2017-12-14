// RUN: %clang_cc1 -emit-llvm -O1 -o - -triple=i386-pc-win32 %s -fexceptions -fcxx-exceptions | FileCheck %s

struct type_info;
namespace std { using ::type_info; }

struct V { virtual void f(); };
struct A : virtual V { A(); };

extern A a;
extern V v;
extern int b;
A* fn();

const std::type_info* test0_typeid() { return &typeid(int); }
// CHECK-LABEL: define %struct.type_info* @"\01?test0_typeid@@YAPBUtype_info@@XZ"()
// CHECK:   ret %struct.type_info* bitcast (%rtti.TypeDescriptor2* @"\01??_R0H@8" to %struct.type_info*)

const std::type_info* test1_typeid() { return &typeid(A); }
// CHECK-LABEL: define %struct.type_info* @"\01?test1_typeid@@YAPBUtype_info@@XZ"()
// CHECK:   ret %struct.type_info* bitcast (%rtti.TypeDescriptor7* @"\01??_R0?AUA@@@8" to %struct.type_info*)

const std::type_info* test2_typeid() { return &typeid(&a); }
// CHECK-LABEL: define %struct.type_info* @"\01?test2_typeid@@YAPBUtype_info@@XZ"()
// CHECK:   ret %struct.type_info* bitcast (%rtti.TypeDescriptor7* @"\01??_R0PAUA@@@8" to %struct.type_info*)

const std::type_info* test3_typeid() { return &typeid(*fn()); }
// CHECK-LABEL: define %struct.type_info* @"\01?test3_typeid@@YAPBUtype_info@@XZ"()
// CHECK:        [[CALL:%.*]] = tail call %struct.A* @"\01?fn@@YAPAUA@@XZ"()
// CHECK-NEXT:   [[CMP:%.*]] = icmp eq %struct.A* [[CALL]], null
// CHECK-NEXT:   br i1 [[CMP]]
// CHECK:        tail call i8* @__RTtypeid(i8* null)
// CHECK-NEXT:   unreachable
// CHECK:        [[THIS:%.*]] = bitcast %struct.A* [[CALL]] to i8*
// CHECK-NEXT:   [[VBTBLP:%.*]] = getelementptr inbounds %struct.A, %struct.A* [[CALL]], i32 0, i32 0
// CHECK-NEXT:   [[VBTBL:%.*]] = load i32*, i32** [[VBTBLP]], align 4
// CHECK-NEXT:   [[VBSLOT:%.*]] = getelementptr inbounds i32, i32* [[VBTBL]], i32 1
// CHECK-NEXT:   [[VBASE_OFFS:%.*]] = load i32, i32* [[VBSLOT]], align 4
// CHECK-NEXT:   [[ADJ:%.*]] = getelementptr inbounds i8, i8* [[THIS]], i32 [[VBASE_OFFS]]
// CHECK-NEXT:   [[RT:%.*]] = tail call i8* @__RTtypeid(i8* [[ADJ]])
// CHECK-NEXT:   [[RET:%.*]] = bitcast i8* [[RT]] to %struct.type_info*
// CHECK-NEXT:   ret %struct.type_info* [[RET]]

const std::type_info* test4_typeid() { return &typeid(b); }
// CHECK: define %struct.type_info* @"\01?test4_typeid@@YAPBUtype_info@@XZ"()
// CHECK:   ret %struct.type_info* bitcast (%rtti.TypeDescriptor2* @"\01??_R0H@8" to %struct.type_info*)

const std::type_info* test5_typeid() { return &typeid(v); }
// CHECK: define %struct.type_info* @"\01?test5_typeid@@YAPBUtype_info@@XZ"()
// CHECK:        [[RT:%.*]] = tail call i8* @__RTtypeid(i8* bitcast (%struct.V* @"\01?v@@3UV@@A" to i8*))
// CHECK-NEXT:   [[RET:%.*]] = bitcast i8* [[RT]] to %struct.type_info*
// CHECK-NEXT:   ret %struct.type_info* [[RET]]

namespace PR26329 {
struct Polymorphic {
  virtual ~Polymorphic();
};

void f(const Polymorphic &poly) {
  try {
    throw;
  } catch (...) {
    Polymorphic cleanup;
    typeid(poly);
  }
}
// CHECK-LABEL: define void @"\01?f@PR26329@@YAXABUPolymorphic@1@@Z"(
// CHECK: %[[cs:.*]] = catchswitch within none [label %{{.*}}] unwind to caller
// CHECK: %[[cp:.*]] = catchpad within %[[cs]] [i8* null, i32 64, i8* null]
// CHECK: invoke i8* @__RTtypeid(i8* {{.*}}) [ "funclet"(token %[[cp]]) ]
}
