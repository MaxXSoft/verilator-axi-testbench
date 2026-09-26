#include "benchmark.h"

/* Keep the upstream workload unchanged. Its two TIME hooks bracket counters;
   the host runner computes scores without requiring a soft-float runtime. */
long time(long *unused) {
  static unsigned calls;
  (void)unused;
  if (calls++ == 0) benchmark_start();
  else benchmark_stop();
  return 0;
}

#define main dhrystone_main
#include "dhry_1.c"
#undef main

int main(void) {
  dhrystone_main();
  if (Int_Glob != 5 || Bool_Glob != 1 || Ch_1_Glob != 'A' || Ch_2_Glob != 'B' ||
      Arr_1_Glob[8] != 7 || Arr_2_Glob[8][7] != DHRY_ITERS + 10 ||
      Ptr_Glob->Ptr_Comp != Next_Ptr_Glob || Next_Ptr_Glob->Ptr_Comp != Next_Ptr_Glob ||
      Ptr_Glob->Discr != Ident_1 || Next_Ptr_Glob->Discr != Ident_1 ||
      Ptr_Glob->variant.var_1.Enum_Comp != Ident_3 ||
      Ptr_Glob->variant.var_1.Int_Comp != 17 ||
      Next_Ptr_Glob->variant.var_1.Enum_Comp != Ident_2 ||
      Next_Ptr_Glob->variant.var_1.Int_Comp != 18 ||
      strcmp(Ptr_Glob->variant.var_1.Str_Comp, "DHRYSTONE PROGRAM, SOME STRING") ||
      strcmp(Next_Ptr_Glob->variant.var_1.Str_Comp, "DHRYSTONE PROGRAM, SOME STRING"))
    return 1;
  benchmark_report("dhrystone", DHRY_ITERS);
  return 0;
}
