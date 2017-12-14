// Test target codegen - host bc file has to be created first.
// RUN: %clang_cc1 -verify -fopenmp -x c++ -triple powerpc64le-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm-bc %s -o %t-ppc-host.bc
// RUN: %clang_cc1 -verify -fopenmp -x c++ -triple nvptx64-unknown-unknown -fopenmp-targets=nvptx64-nvidia-cuda -emit-llvm %s -fopenmp-is-device -fopenmp-host-ir-file-path %t-ppc-host.bc -o - | FileCheck %s --check-prefix CHECK --check-prefix CHECK-64
// RUN: %clang_cc1 -verify -fopenmp -x c++ -triple i386-unknown-unknown -fopenmp-targets=nvptx-nvidia-cuda -emit-llvm-bc %s -o %t-x86-host.bc
// RUN: %clang_cc1 -verify -fopenmp -x c++ -triple nvptx-unknown-unknown -fopenmp-targets=nvptx-nvidia-cuda -emit-llvm %s -fopenmp-is-device -fopenmp-host-ir-file-path %t-x86-host.bc -o - | FileCheck %s --check-prefix CHECK --check-prefix CHECK-32
// RUN: %clang_cc1 -verify -fopenmp -fexceptions -fcxx-exceptions -x c++ -triple nvptx-unknown-unknown -fopenmp-targets=nvptx-nvidia-cuda -emit-llvm %s -fopenmp-is-device -fopenmp-host-ir-file-path %t-x86-host.bc -o - | FileCheck %s --check-prefix CHECK --check-prefix CHECK-32
// expected-no-diagnostics
#ifndef HEADER
#define HEADER

// Check that the execution mode of all 6 target regions is set to Generic Mode.
// CHECK-DAG: {{@__omp_offloading_.+l100}}_exec_mode = weak constant i8 1
// CHECK-DAG: {{@__omp_offloading_.+l177}}_exec_mode = weak constant i8 1
// CHECK-DAG: {{@__omp_offloading_.+l287}}_exec_mode = weak constant i8 1
// CHECK-DAG: {{@__omp_offloading_.+l324}}_exec_mode = weak constant i8 1
// CHECK-DAG: {{@__omp_offloading_.+l342}}_exec_mode = weak constant i8 1
// CHECK-DAG: {{@__omp_offloading_.+l307}}_exec_mode = weak constant i8 1

__thread int id;

template<typename tx, typename ty>
struct TT{
  tx X;
  ty Y;
};

int foo(int n) {
  int a = 0;
  short aa = 0;
  float b[10];
  float bn[n];
  double c[5][10];
  double cn[5][n];
  TT<long long, char> d;

  // CHECK-LABEL: define {{.*}}void {{@__omp_offloading_.+foo.+l100}}_worker()
  // CHECK-DAG: [[OMP_EXEC_STATUS:%.+]] = alloca i8,
  // CHECK-DAG: [[OMP_WORK_FN:%.+]] = alloca i8*,
  // CHECK: store i8* null, i8** [[OMP_WORK_FN]],
  // CHECK: store i8 0, i8* [[OMP_EXEC_STATUS]],
  // CHECK: br label {{%?}}[[AWAIT_WORK:.+]]
  //
  // CHECK: [[AWAIT_WORK]]
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: [[WORK:%.+]] = load i8*, i8** [[OMP_WORK_FN]],
  // CHECK: [[SHOULD_EXIT:%.+]] = icmp eq i8* [[WORK]], null
  // CHECK: br i1 [[SHOULD_EXIT]], label {{%?}}[[EXIT:.+]], label {{%?}}[[SEL_WORKERS:.+]]
  //
  // CHECK: [[SEL_WORKERS]]
  // CHECK: [[ST:%.+]] = load i8, i8* [[OMP_EXEC_STATUS]],
  // CHECK: [[IS_ACTIVE:%.+]] = icmp ne i8 [[ST]], 0
  // CHECK: br i1 [[IS_ACTIVE]], label {{%?}}[[EXEC_PARALLEL:.+]], label {{%?}}[[BAR_PARALLEL:.+]]
  //
  // CHECK: [[EXEC_PARALLEL]]
  // CHECK: br label {{%?}}[[TERM_PARALLEL:.+]]
  //
  // CHECK: [[TERM_PARALLEL]]
  // CHECK: br label {{%?}}[[BAR_PARALLEL]]
  //
  // CHECK: [[BAR_PARALLEL]]
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: br label {{%?}}[[AWAIT_WORK]]
  //
  // CHECK: [[EXIT]]
  // CHECK: ret void

  // CHECK: define {{.*}}void [[T1:@__omp_offloading_.+foo.+l100]]()
  // CHECK-DAG: [[TID:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  // CHECK-DAG: [[NTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[WS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK-DAG: [[TH_LIMIT:%.+]] = sub i32 [[NTH]], [[WS]]
  // CHECK: [[IS_WORKER:%.+]] = icmp ult i32 [[TID]], [[TH_LIMIT]]
  // CHECK: br i1 [[IS_WORKER]], label {{%?}}[[WORKER:.+]], label {{%?}}[[CHECK_MASTER:.+]]
  //
  // CHECK: [[WORKER]]
  // CHECK: {{call|invoke}} void [[T1]]_worker()
  // CHECK: br label {{%?}}[[EXIT:.+]]
  //
  // CHECK: [[CHECK_MASTER]]
  // CHECK-DAG: [[CMTID:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  // CHECK-DAG: [[CMNTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[CMWS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK: [[IS_MASTER:%.+]] = icmp eq i32 [[CMTID]],
  // CHECK: br i1 [[IS_MASTER]], label {{%?}}[[MASTER:.+]], label {{%?}}[[EXIT]]
  //
  // CHECK: [[MASTER]]
  // CHECK-DAG: [[MNTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[MWS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK: [[MTMP1:%.+]] = sub i32 [[MNTH]], [[MWS]]
  // CHECK: call void @__kmpc_kernel_init(i32 [[MTMP1]]
  // CHECK: br label {{%?}}[[TERMINATE:.+]]
  //
  // CHECK: [[TERMINATE]]
  // CHECK: call void @__kmpc_kernel_deinit()
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: br label {{%?}}[[EXIT]]
  //
  // CHECK: [[EXIT]]
  // CHECK: ret void
  #pragma omp target
  {
  }

  // CHECK-NOT: define {{.*}}void [[T2:@__omp_offloading_.+foo.+]]_worker()
  #pragma omp target if(0)
  {
  }

  // CHECK-LABEL: define {{.*}}void {{@__omp_offloading_.+foo.+l177}}_worker()
  // CHECK-DAG: [[OMP_EXEC_STATUS:%.+]] = alloca i8,
  // CHECK-DAG: [[OMP_WORK_FN:%.+]] = alloca i8*,
  // CHECK: store i8* null, i8** [[OMP_WORK_FN]],
  // CHECK: store i8 0, i8* [[OMP_EXEC_STATUS]],
  // CHECK: br label {{%?}}[[AWAIT_WORK:.+]]
  //
  // CHECK: [[AWAIT_WORK]]
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: [[WORK:%.+]] = load i8*, i8** [[OMP_WORK_FN]],
  // CHECK: [[SHOULD_EXIT:%.+]] = icmp eq i8* [[WORK]], null
  // CHECK: br i1 [[SHOULD_EXIT]], label {{%?}}[[EXIT:.+]], label {{%?}}[[SEL_WORKERS:.+]]
  //
  // CHECK: [[SEL_WORKERS]]
  // CHECK: [[ST:%.+]] = load i8, i8* [[OMP_EXEC_STATUS]],
  // CHECK: [[IS_ACTIVE:%.+]] = icmp ne i8 [[ST]], 0
  // CHECK: br i1 [[IS_ACTIVE]], label {{%?}}[[EXEC_PARALLEL:.+]], label {{%?}}[[BAR_PARALLEL:.+]]
  //
  // CHECK: [[EXEC_PARALLEL]]
  // CHECK: br label {{%?}}[[TERM_PARALLEL:.+]]
  //
  // CHECK: [[TERM_PARALLEL]]
  // CHECK: br label {{%?}}[[BAR_PARALLEL]]
  //
  // CHECK: [[BAR_PARALLEL]]
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: br label {{%?}}[[AWAIT_WORK]]
  //
  // CHECK: [[EXIT]]
  // CHECK: ret void

  // CHECK: define {{.*}}void [[T2:@__omp_offloading_.+foo.+l177]](i[[SZ:32|64]] [[ARG1:%[a-zA-Z_]+]], i[[SZ:32|64]] [[ID:%[a-zA-Z_]+]])
  // CHECK: [[AA_ADDR:%.+]] = alloca i[[SZ]],
  // CHECK: store i[[SZ]] [[ARG1]], i[[SZ]]* [[AA_ADDR]],
  // CHECK: [[AA_CADDR:%.+]] = bitcast i[[SZ]]* [[AA_ADDR]] to i16*
  // CHECK-DAG: [[TID:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  // CHECK-DAG: [[NTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[WS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK-DAG: [[TH_LIMIT:%.+]] = sub i32 [[NTH]], [[WS]]
  // CHECK: [[IS_WORKER:%.+]] = icmp ult i32 [[TID]], [[TH_LIMIT]]
  // CHECK: br i1 [[IS_WORKER]], label {{%?}}[[WORKER:.+]], label {{%?}}[[CHECK_MASTER:.+]]
  //
  // CHECK: [[WORKER]]
  // CHECK: {{call|invoke}} void [[T2]]_worker()
  // CHECK: br label {{%?}}[[EXIT:.+]]
  //
  // CHECK: [[CHECK_MASTER]]
  // CHECK-DAG: [[CMTID:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  // CHECK-DAG: [[CMNTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[CMWS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK: [[IS_MASTER:%.+]] = icmp eq i32 [[CMTID]],
  // CHECK: br i1 [[IS_MASTER]], label {{%?}}[[MASTER:.+]], label {{%?}}[[EXIT]]
  //
  // CHECK: [[MASTER]]
  // CHECK-DAG: [[MNTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[MWS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK: [[MTMP1:%.+]] = sub i32 [[MNTH]], [[MWS]]
  // CHECK: call void @__kmpc_kernel_init(i32 [[MTMP1]]
  // CHECK: load i16, i16* [[AA_CADDR]],
  // CHECK: br label {{%?}}[[TERMINATE:.+]]
  //
  // CHECK: [[TERMINATE]]
  // CHECK: call void @__kmpc_kernel_deinit()
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: br label {{%?}}[[EXIT]]
  //
  // CHECK: [[EXIT]]
  // CHECK: ret void
  #pragma omp target if(1)
  {
    aa += 1;
    id = aa;
  }

  // CHECK-LABEL: define {{.*}}void {{@__omp_offloading_.+foo.+l287}}_worker()
  // CHECK-DAG: [[OMP_EXEC_STATUS:%.+]] = alloca i8,
  // CHECK-DAG: [[OMP_WORK_FN:%.+]] = alloca i8*,
  // CHECK: store i8* null, i8** [[OMP_WORK_FN]],
  // CHECK: store i8 0, i8* [[OMP_EXEC_STATUS]],
  // CHECK: br label {{%?}}[[AWAIT_WORK:.+]]
  //
  // CHECK: [[AWAIT_WORK]]
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: [[WORK:%.+]] = load i8*, i8** [[OMP_WORK_FN]],
  // CHECK: [[SHOULD_EXIT:%.+]] = icmp eq i8* [[WORK]], null
  // CHECK: br i1 [[SHOULD_EXIT]], label {{%?}}[[EXIT:.+]], label {{%?}}[[SEL_WORKERS:.+]]
  //
  // CHECK: [[SEL_WORKERS]]
  // CHECK: [[ST:%.+]] = load i8, i8* [[OMP_EXEC_STATUS]],
  // CHECK: [[IS_ACTIVE:%.+]] = icmp ne i8 [[ST]], 0
  // CHECK: br i1 [[IS_ACTIVE]], label {{%?}}[[EXEC_PARALLEL:.+]], label {{%?}}[[BAR_PARALLEL:.+]]
  //
  // CHECK: [[EXEC_PARALLEL]]
  // CHECK: br label {{%?}}[[TERM_PARALLEL:.+]]
  //
  // CHECK: [[TERM_PARALLEL]]
  // CHECK: br label {{%?}}[[BAR_PARALLEL]]
  //
  // CHECK: [[BAR_PARALLEL]]
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: br label {{%?}}[[AWAIT_WORK]]
  //
  // CHECK: [[EXIT]]
  // CHECK: ret void

  // CHECK: define {{.*}}void [[T3:@__omp_offloading_.+foo.+l287]](i[[SZ]]
  // Create local storage for each capture.
  // CHECK:    [[LOCAL_A:%.+]] = alloca i[[SZ]]
  // CHECK:    [[LOCAL_B:%.+]] = alloca [10 x float]*
  // CHECK:    [[LOCAL_VLA1:%.+]] = alloca i[[SZ]]
  // CHECK:    [[LOCAL_BN:%.+]] = alloca float*
  // CHECK:    [[LOCAL_C:%.+]] = alloca [5 x [10 x double]]*
  // CHECK:    [[LOCAL_VLA2:%.+]] = alloca i[[SZ]]
  // CHECK:    [[LOCAL_VLA3:%.+]] = alloca i[[SZ]]
  // CHECK:    [[LOCAL_CN:%.+]] = alloca double*
  // CHECK:    [[LOCAL_D:%.+]] = alloca [[TT:%.+]]*
  // CHECK-DAG: store i[[SZ]] [[ARG_A:%.+]], i[[SZ]]* [[LOCAL_A]]
  // CHECK-DAG: store [10 x float]* [[ARG_B:%.+]], [10 x float]** [[LOCAL_B]]
  // CHECK-DAG: store i[[SZ]] [[ARG_VLA1:%.+]], i[[SZ]]* [[LOCAL_VLA1]]
  // CHECK-DAG: store float* [[ARG_BN:%.+]], float** [[LOCAL_BN]]
  // CHECK-DAG: store [5 x [10 x double]]* [[ARG_C:%.+]], [5 x [10 x double]]** [[LOCAL_C]]
  // CHECK-DAG: store i[[SZ]] [[ARG_VLA2:%.+]], i[[SZ]]* [[LOCAL_VLA2]]
  // CHECK-DAG: store i[[SZ]] [[ARG_VLA3:%.+]], i[[SZ]]* [[LOCAL_VLA3]]
  // CHECK-DAG: store double* [[ARG_CN:%.+]], double** [[LOCAL_CN]]
  // CHECK-DAG: store [[TT]]* [[ARG_D:%.+]], [[TT]]** [[LOCAL_D]]
  //
  // CHECK-64-DAG: [[REF_A:%.+]] = bitcast i64* [[LOCAL_A]] to i32*
  // CHECK-DAG:    [[REF_B:%.+]] = load [10 x float]*, [10 x float]** [[LOCAL_B]],
  // CHECK-DAG:    [[VAL_VLA1:%.+]] = load i[[SZ]], i[[SZ]]* [[LOCAL_VLA1]],
  // CHECK-DAG:    [[REF_BN:%.+]] = load float*, float** [[LOCAL_BN]],
  // CHECK-DAG:    [[REF_C:%.+]] = load [5 x [10 x double]]*, [5 x [10 x double]]** [[LOCAL_C]],
  // CHECK-DAG:    [[VAL_VLA2:%.+]] = load i[[SZ]], i[[SZ]]* [[LOCAL_VLA2]],
  // CHECK-DAG:    [[VAL_VLA3:%.+]] = load i[[SZ]], i[[SZ]]* [[LOCAL_VLA3]],
  // CHECK-DAG:    [[REF_CN:%.+]] = load double*, double** [[LOCAL_CN]],
  // CHECK-DAG:    [[REF_D:%.+]] = load [[TT]]*, [[TT]]** [[LOCAL_D]],
  //
  // CHECK-DAG: [[TID:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  // CHECK-DAG: [[NTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[WS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK-DAG: [[TH_LIMIT:%.+]] = sub i32 [[NTH]], [[WS]]
  // CHECK: [[IS_WORKER:%.+]] = icmp ult i32 [[TID]], [[TH_LIMIT]]
  // CHECK: br i1 [[IS_WORKER]], label {{%?}}[[WORKER:.+]], label {{%?}}[[CHECK_MASTER:.+]]
  //
  // CHECK: [[WORKER]]
  // CHECK: {{call|invoke}} void [[T3]]_worker()
  // CHECK: br label {{%?}}[[EXIT:.+]]
  //
  // CHECK: [[CHECK_MASTER]]
  // CHECK-DAG: [[CMTID:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  // CHECK-DAG: [[CMNTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[CMWS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK: [[IS_MASTER:%.+]] = icmp eq i32 [[CMTID]],
  // CHECK: br i1 [[IS_MASTER]], label {{%?}}[[MASTER:.+]], label {{%?}}[[EXIT]]
  //
  // CHECK: [[MASTER]]
  // CHECK-DAG: [[MNTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[MWS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK: [[MTMP1:%.+]] = sub i32 [[MNTH]], [[MWS]]
  // CHECK: call void @__kmpc_kernel_init(i32 [[MTMP1]]
  //
  // Use captures.
  // CHECK-64-DAG:  load i32, i32* [[REF_A]]
  // CHECK-32-DAG:  load i32, i32* [[LOCAL_A]]
  // CHECK-DAG:  getelementptr inbounds [10 x float], [10 x float]* [[REF_B]], i[[SZ]] 0, i[[SZ]] 2
  // CHECK-DAG:  getelementptr inbounds float, float* [[REF_BN]], i[[SZ]] 3
  // CHECK-DAG:  getelementptr inbounds [5 x [10 x double]], [5 x [10 x double]]* [[REF_C]], i[[SZ]] 0, i[[SZ]] 1
  // CHECK-DAG:  getelementptr inbounds double, double* [[REF_CN]], i[[SZ]] %{{.+}}
  // CHECK-DAG:     getelementptr inbounds [[TT]], [[TT]]* [[REF_D]], i32 0, i32 0
  //
  // CHECK: br label {{%?}}[[TERMINATE:.+]]
  //
  // CHECK: [[TERMINATE]]
  // CHECK: call void @__kmpc_kernel_deinit()
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: br label {{%?}}[[EXIT]]
  //
  // CHECK: [[EXIT]]
  // CHECK: ret void
  #pragma omp target if(n>20)
  {
    a += 1;
    b[2] += 1.0;
    bn[3] += 1.0;
    c[1][2] += 1.0;
    cn[1][3] += 1.0;
    d.X += 1;
    d.Y += 1;
  }

  return a;
}

template<typename tx>
tx ftemplate(int n) {
  tx a = 0;
  short aa = 0;
  tx b[10];

  #pragma omp target if(n>40)
  {
    a += 1;
    aa += 1;
    b[2] += 1;
  }

  return a;
}

static
int fstatic(int n) {
  int a = 0;
  short aa = 0;
  char aaa = 0;
  int b[10];

  #pragma omp target if(n>50)
  {
    a += 1;
    aa += 1;
    aaa += 1;
    b[2] += 1;
  }

  return a;
}

struct S1 {
  double a;

  int r1(int n){
    int b = n+1;
    short int c[2][n];

    #pragma omp target if(n>60)
    {
      this->a = (double)b + 1.5;
      c[1][1] = ++a;
    }

    return c[1][1] + (int)b;
  }
};

int bar(int n){
  int a = 0;

  a += foo(n);

  S1 S;
  a += S.r1(n);

  a += fstatic(n);

  a += ftemplate<int>(n);

  return a;
}

  // CHECK-LABEL: define {{.*}}void {{@__omp_offloading_.+static.+324}}_worker()
  // CHECK-DAG: [[OMP_EXEC_STATUS:%.+]] = alloca i8,
  // CHECK-DAG: [[OMP_WORK_FN:%.+]] = alloca i8*,
  // CHECK: store i8* null, i8** [[OMP_WORK_FN]],
  // CHECK: store i8 0, i8* [[OMP_EXEC_STATUS]],
  // CHECK: br label {{%?}}[[AWAIT_WORK:.+]]
  //
  // CHECK: [[AWAIT_WORK]]
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: [[WORK:%.+]] = load i8*, i8** [[OMP_WORK_FN]],
  // CHECK: [[SHOULD_EXIT:%.+]] = icmp eq i8* [[WORK]], null
  // CHECK: br i1 [[SHOULD_EXIT]], label {{%?}}[[EXIT:.+]], label {{%?}}[[SEL_WORKERS:.+]]
  //
  // CHECK: [[SEL_WORKERS]]
  // CHECK: [[ST:%.+]] = load i8, i8* [[OMP_EXEC_STATUS]],
  // CHECK: [[IS_ACTIVE:%.+]] = icmp ne i8 [[ST]], 0
  // CHECK: br i1 [[IS_ACTIVE]], label {{%?}}[[EXEC_PARALLEL:.+]], label {{%?}}[[BAR_PARALLEL:.+]]
  //
  // CHECK: [[EXEC_PARALLEL]]
  // CHECK: br label {{%?}}[[TERM_PARALLEL:.+]]
  //
  // CHECK: [[TERM_PARALLEL]]
  // CHECK: br label {{%?}}[[BAR_PARALLEL]]
  //
  // CHECK: [[BAR_PARALLEL]]
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: br label {{%?}}[[AWAIT_WORK]]
  //
  // CHECK: [[EXIT]]
  // CHECK: ret void

  // CHECK: define {{.*}}void [[T4:@__omp_offloading_.+static.+l324]](i[[SZ]]
  // Create local storage for each capture.
  // CHECK:  [[LOCAL_A:%.+]] = alloca i[[SZ]]
  // CHECK:  [[LOCAL_AA:%.+]] = alloca i[[SZ]]
  // CHECK:  [[LOCAL_AAA:%.+]] = alloca i[[SZ]]
  // CHECK:  [[LOCAL_B:%.+]] = alloca [10 x i32]*
  // CHECK-DAG:  store i[[SZ]] [[ARG_A:%.+]], i[[SZ]]* [[LOCAL_A]]
  // CHECK-DAG:  store i[[SZ]] [[ARG_AA:%.+]], i[[SZ]]* [[LOCAL_AA]]
  // CHECK-DAG:  store i[[SZ]] [[ARG_AAA:%.+]], i[[SZ]]* [[LOCAL_AAA]]
  // CHECK-DAG:  store [10 x i32]* [[ARG_B:%.+]], [10 x i32]** [[LOCAL_B]]
  // Store captures in the context.
  // CHECK-64-DAG:   [[REF_A:%.+]] = bitcast i[[SZ]]* [[LOCAL_A]] to i32*
  // CHECK-DAG:      [[REF_AA:%.+]] = bitcast i[[SZ]]* [[LOCAL_AA]] to i16*
  // CHECK-DAG:      [[REF_AAA:%.+]] = bitcast i[[SZ]]* [[LOCAL_AAA]] to i8*
  // CHECK-DAG:      [[REF_B:%.+]] = load [10 x i32]*, [10 x i32]** [[LOCAL_B]],
  //
  // CHECK-DAG: [[TID:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  // CHECK-DAG: [[NTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[WS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK-DAG: [[TH_LIMIT:%.+]] = sub i32 [[NTH]], [[WS]]
  // CHECK: [[IS_WORKER:%.+]] = icmp ult i32 [[TID]], [[TH_LIMIT]]
  // CHECK: br i1 [[IS_WORKER]], label {{%?}}[[WORKER:.+]], label {{%?}}[[CHECK_MASTER:.+]]
  //
  // CHECK: [[WORKER]]
  // CHECK: {{call|invoke}} void [[T4]]_worker()
  // CHECK: br label {{%?}}[[EXIT:.+]]
  //
  // CHECK: [[CHECK_MASTER]]
  // CHECK-DAG: [[CMTID:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  // CHECK-DAG: [[CMNTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[CMWS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK: [[IS_MASTER:%.+]] = icmp eq i32 [[CMTID]],
  // CHECK: br i1 [[IS_MASTER]], label {{%?}}[[MASTER:.+]], label {{%?}}[[EXIT]]
  //
  // CHECK: [[MASTER]]
  // CHECK-DAG: [[MNTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[MWS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK: [[MTMP1:%.+]] = sub i32 [[MNTH]], [[MWS]]
  // CHECK: call void @__kmpc_kernel_init(i32 [[MTMP1]]
  // CHECK-64-DAG: load i32, i32* [[REF_A]]
  // CHECK-32-DAG: load i32, i32* [[LOCAL_A]]
  // CHECK-DAG:    load i16, i16* [[REF_AA]]
  // CHECK-DAG:    getelementptr inbounds [10 x i32], [10 x i32]* [[REF_B]], i[[SZ]] 0, i[[SZ]] 2
  // CHECK: br label {{%?}}[[TERMINATE:.+]]
  //
  // CHECK: [[TERMINATE]]
  // CHECK: call void @__kmpc_kernel_deinit()
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: br label {{%?}}[[EXIT]]
  //
  // CHECK: [[EXIT]]
  // CHECK: ret void



  // CHECK-LABEL: define {{.*}}void {{@__omp_offloading_.+S1.+l342}}_worker()
  // CHECK-DAG: [[OMP_EXEC_STATUS:%.+]] = alloca i8,
  // CHECK-DAG: [[OMP_WORK_FN:%.+]] = alloca i8*,
  // CHECK: store i8* null, i8** [[OMP_WORK_FN]],
  // CHECK: store i8 0, i8* [[OMP_EXEC_STATUS]],
  // CHECK: br label {{%?}}[[AWAIT_WORK:.+]]
  //
  // CHECK: [[AWAIT_WORK]]
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: [[WORK:%.+]] = load i8*, i8** [[OMP_WORK_FN]],
  // CHECK: [[SHOULD_EXIT:%.+]] = icmp eq i8* [[WORK]], null
  // CHECK: br i1 [[SHOULD_EXIT]], label {{%?}}[[EXIT:.+]], label {{%?}}[[SEL_WORKERS:.+]]
  //
  // CHECK: [[SEL_WORKERS]]
  // CHECK: [[ST:%.+]] = load i8, i8* [[OMP_EXEC_STATUS]],
  // CHECK: [[IS_ACTIVE:%.+]] = icmp ne i8 [[ST]], 0
  // CHECK: br i1 [[IS_ACTIVE]], label {{%?}}[[EXEC_PARALLEL:.+]], label {{%?}}[[BAR_PARALLEL:.+]]
  //
  // CHECK: [[EXEC_PARALLEL]]
  // CHECK: br label {{%?}}[[TERM_PARALLEL:.+]]
  //
  // CHECK: [[TERM_PARALLEL]]
  // CHECK: br label {{%?}}[[BAR_PARALLEL]]
  //
  // CHECK: [[BAR_PARALLEL]]
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: br label {{%?}}[[AWAIT_WORK]]
  //
  // CHECK: [[EXIT]]
  // CHECK: ret void

  // CHECK: define {{.*}}void [[T5:@__omp_offloading_.+S1.+l342]](
  // Create local storage for each capture.
  // CHECK:       [[LOCAL_THIS:%.+]] = alloca [[S1:%struct.*]]*
  // CHECK:       [[LOCAL_B:%.+]] = alloca i[[SZ]]
  // CHECK:       [[LOCAL_VLA1:%.+]] = alloca i[[SZ]]
  // CHECK:       [[LOCAL_VLA2:%.+]] = alloca i[[SZ]]
  // CHECK:       [[LOCAL_C:%.+]] = alloca i16*
  // CHECK-DAG:   store [[S1]]* [[ARG_THIS:%.+]], [[S1]]** [[LOCAL_THIS]]
  // CHECK-DAG:   store i[[SZ]] [[ARG_B:%.+]], i[[SZ]]* [[LOCAL_B]]
  // CHECK-DAG:   store i[[SZ]] [[ARG_VLA1:%.+]], i[[SZ]]* [[LOCAL_VLA1]]
  // CHECK-DAG:   store i[[SZ]] [[ARG_VLA2:%.+]], i[[SZ]]* [[LOCAL_VLA2]]
  // CHECK-DAG:   store i16* [[ARG_C:%.+]], i16** [[LOCAL_C]]
  // Store captures in the context.
  // CHECK-DAG:   [[REF_THIS:%.+]] = load [[S1]]*, [[S1]]** [[LOCAL_THIS]],
  // CHECK-64-DAG:[[REF_B:%.+]] = bitcast i[[SZ]]* [[LOCAL_B]] to i32*
  // CHECK-DAG:   [[VAL_VLA1:%.+]] = load i[[SZ]], i[[SZ]]* [[LOCAL_VLA1]],
  // CHECK-DAG:   [[VAL_VLA2:%.+]] = load i[[SZ]], i[[SZ]]* [[LOCAL_VLA2]],
  // CHECK-DAG:   [[REF_C:%.+]] = load i16*, i16** [[LOCAL_C]],
  //
  // CHECK-DAG: [[TID:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  // CHECK-DAG: [[NTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[WS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK-DAG: [[TH_LIMIT:%.+]] = sub i32 [[NTH]], [[WS]]
  // CHECK: [[IS_WORKER:%.+]] = icmp ult i32 [[TID]], [[TH_LIMIT]]
  // CHECK: br i1 [[IS_WORKER]], label {{%?}}[[WORKER:.+]], label {{%?}}[[CHECK_MASTER:.+]]
  //
  // CHECK: [[WORKER]]
  // CHECK: {{call|invoke}} void [[T5]]_worker()
  // CHECK: br label {{%?}}[[EXIT:.+]]
  //
  // CHECK: [[CHECK_MASTER]]
  // CHECK-DAG: [[CMTID:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  // CHECK-DAG: [[CMNTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[CMWS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK: [[IS_MASTER:%.+]] = icmp eq i32 [[CMTID]],
  // CHECK: br i1 [[IS_MASTER]], label {{%?}}[[MASTER:.+]], label {{%?}}[[EXIT]]
  //
  // CHECK: [[MASTER]]
  // CHECK-DAG: [[MNTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[MWS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK: [[MTMP1:%.+]] = sub i32 [[MNTH]], [[MWS]]
  // CHECK: call void @__kmpc_kernel_init(i32 [[MTMP1]]
  // Use captures.
  // CHECK-DAG:   getelementptr inbounds [[S1]], [[S1]]* [[REF_THIS]], i32 0, i32 0
  // CHECK-64-DAG:load i32, i32* [[REF_B]]
  // CHECK-32-DAG:load i32, i32* [[LOCAL_B]]
  // CHECK-DAG:   getelementptr inbounds i16, i16* [[REF_C]], i[[SZ]] %{{.+}}
  // CHECK: br label {{%?}}[[TERMINATE:.+]]
  //
  // CHECK: [[TERMINATE]]
  // CHECK: call void @__kmpc_kernel_deinit()
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: br label {{%?}}[[EXIT]]
  //
  // CHECK: [[EXIT]]
  // CHECK: ret void



  // CHECK-LABEL: define {{.*}}void {{@__omp_offloading_.+template.+l307}}_worker()
  // CHECK-DAG: [[OMP_EXEC_STATUS:%.+]] = alloca i8,
  // CHECK-DAG: [[OMP_WORK_FN:%.+]] = alloca i8*,
  // CHECK: store i8* null, i8** [[OMP_WORK_FN]],
  // CHECK: store i8 0, i8* [[OMP_EXEC_STATUS]],
  // CHECK: br label {{%?}}[[AWAIT_WORK:.+]]
  //
  // CHECK: [[AWAIT_WORK]]
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: [[WORK:%.+]] = load i8*, i8** [[OMP_WORK_FN]],
  // CHECK: [[SHOULD_EXIT:%.+]] = icmp eq i8* [[WORK]], null
  // CHECK: br i1 [[SHOULD_EXIT]], label {{%?}}[[EXIT:.+]], label {{%?}}[[SEL_WORKERS:.+]]
  //
  // CHECK: [[SEL_WORKERS]]
  // CHECK: [[ST:%.+]] = load i8, i8* [[OMP_EXEC_STATUS]],
  // CHECK: [[IS_ACTIVE:%.+]] = icmp ne i8 [[ST]], 0
  // CHECK: br i1 [[IS_ACTIVE]], label {{%?}}[[EXEC_PARALLEL:.+]], label {{%?}}[[BAR_PARALLEL:.+]]
  //
  // CHECK: [[EXEC_PARALLEL]]
  // CHECK: br label {{%?}}[[TERM_PARALLEL:.+]]
  //
  // CHECK: [[TERM_PARALLEL]]
  // CHECK: br label {{%?}}[[BAR_PARALLEL]]
  //
  // CHECK: [[BAR_PARALLEL]]
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: br label {{%?}}[[AWAIT_WORK]]
  //
  // CHECK: [[EXIT]]
  // CHECK: ret void

  // CHECK: define {{.*}}void [[T6:@__omp_offloading_.+template.+l307]](i[[SZ]]
  // Create local storage for each capture.
  // CHECK:  [[LOCAL_A:%.+]] = alloca i[[SZ]]
  // CHECK:  [[LOCAL_AA:%.+]] = alloca i[[SZ]]
  // CHECK:  [[LOCAL_B:%.+]] = alloca [10 x i32]*
  // CHECK-DAG:  store i[[SZ]] [[ARG_A:%.+]], i[[SZ]]* [[LOCAL_A]]
  // CHECK-DAG:  store i[[SZ]] [[ARG_AA:%.+]], i[[SZ]]* [[LOCAL_AA]]
  // CHECK-DAG:   store [10 x i32]* [[ARG_B:%.+]], [10 x i32]** [[LOCAL_B]]
  // Store captures in the context.
  // CHECK-64-DAG:[[REF_A:%.+]] = bitcast i[[SZ]]* [[LOCAL_A]] to i32*
  // CHECK-DAG:   [[REF_AA:%.+]] = bitcast i[[SZ]]* [[LOCAL_AA]] to i16*
  // CHECK-DAG:   [[REF_B:%.+]] = load [10 x i32]*, [10 x i32]** [[LOCAL_B]],
  //
  // CHECK-DAG: [[TID:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  // CHECK-DAG: [[NTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[WS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK-DAG: [[TH_LIMIT:%.+]] = sub i32 [[NTH]], [[WS]]
  // CHECK: [[IS_WORKER:%.+]] = icmp ult i32 [[TID]], [[TH_LIMIT]]
  // CHECK: br i1 [[IS_WORKER]], label {{%?}}[[WORKER:.+]], label {{%?}}[[CHECK_MASTER:.+]]
  //
  // CHECK: [[WORKER]]
  // CHECK: {{call|invoke}} void [[T6]]_worker()
  // CHECK: br label {{%?}}[[EXIT:.+]]
  //
  // CHECK: [[CHECK_MASTER]]
  // CHECK-DAG: [[CMTID:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  // CHECK-DAG: [[CMNTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[CMWS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK: [[IS_MASTER:%.+]] = icmp eq i32 [[CMTID]],
  // CHECK: br i1 [[IS_MASTER]], label {{%?}}[[MASTER:.+]], label {{%?}}[[EXIT]]
  //
  // CHECK: [[MASTER]]
  // CHECK-DAG: [[MNTH:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  // CHECK-DAG: [[MWS:%.+]] = call i32 @llvm.nvvm.read.ptx.sreg.warpsize()
  // CHECK: [[MTMP1:%.+]] = sub i32 [[MNTH]], [[MWS]]
  // CHECK: call void @__kmpc_kernel_init(i32 [[MTMP1]]
  //
  // CHECK-64-DAG: load i32, i32* [[REF_A]]
  // CHECK-32-DAG: load i32, i32* [[LOCAL_A]]
  // CHECK-DAG:    load i16, i16* [[REF_AA]]
  // CHECK-DAG:    getelementptr inbounds [10 x i32], [10 x i32]* [[REF_B]], i[[SZ]] 0, i[[SZ]] 2
  //
  // CHECK: br label {{%?}}[[TERMINATE:.+]]
  //
  // CHECK: [[TERMINATE]]
  // CHECK: call void @__kmpc_kernel_deinit()
  // CHECK: call void @llvm.nvvm.barrier0()
  // CHECK: br label {{%?}}[[EXIT]]
  //
  // CHECK: [[EXIT]]
  // CHECK: ret void
#endif
