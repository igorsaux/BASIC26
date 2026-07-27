// Copyright (C) 2026 Igor Spichkin
// SPDX-License-Identifier: MPL-2.0

#include "common.h"

basic26_Vm *vm = NULL;
basic26_State *state = NULL;
basic26_Script *script = NULL;

int main(int argc, const char **argv) {
  UNUSED(argc);
  UNUSED(argv);

  CHECK(basic26_Vm_create(NULL, &vm) == BASIC26_RESULT_OK);
  CHECK(basic26_State_create(vm, &state) == BASIC26_RESULT_OK);
  CHECK(basic26_Script_create(vm, &script) == BASIC26_RESULT_OK);
}
