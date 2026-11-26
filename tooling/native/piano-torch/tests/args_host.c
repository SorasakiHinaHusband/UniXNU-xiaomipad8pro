// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../piano-torch-args.h"
#include <assert.h>

int main(void)
{
  struct arguments args;
  assert(parse_arguments(1, (char *[]){"piano-torch"}, &args) && args.command == STATUS);
  assert(parse_arguments(2, (char *[]){"piano-torch", "status"}, &args) && args.command == STATUS);
  assert(parse_arguments(2, (char *[]){"piano-torch", "toggle"}, &args) && args.command == TOGGLE);
  assert(parse_arguments(2, (char *[]){"piano-torch", "off"}, &args) && args.command == OFF && args.ma == 0);
  assert(parse_arguments(2, (char *[]){"piano-torch", "on"}, &args) &&
         args.command == ON && args.ma == -1 && args.seconds == 0);
  assert(parse_arguments(4, (char *[]){"piano-torch", "on", "45", "12"}, &args) &&
         args.ma == 45 && args.seconds == 12);
  assert(parse_arguments(4, (char *[]){"piano-torch", "on", "100", "0"}, &args) &&
         args.ma == 100 && args.seconds == 0);
  assert(parse_arguments(4, (char *[]){"piano-torch", "on", "101", "1801"}, &args) &&
         args.ma == 100 && args.seconds == 1800);
  char oversized[] = "184467440737095516160";
  assert(parse_arguments(4, (char *[]){"piano-torch", "on", oversized, oversized}, &args) &&
         args.ma == 100 && args.seconds == 1800);
  assert(!parse_arguments(3, (char *[]){"piano-torch", "off", "1"}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "status", "1"}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "toggle", "1"}, &args));
  assert(!parse_arguments(5, (char *[]){"piano-torch", "on", "100", "1", "2"}, &args));
  assert(!parse_arguments(2, (char *[]){"piano-torch", "statuss"}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "on", "0"}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "on", ""}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "on", " 5"}, &args));
  assert(!parse_arguments(3, (char *[]){"piano-torch", "on", "-5"}, &args));
  assert(!parse_arguments(4, (char *[]){"piano-torch", "on", "50", ""}, &args));
  assert(!parse_arguments(4, (char *[]){"piano-torch", "on", "50", "-1"}, &args));
  assert(!parse_arguments(4, (char *[]){"piano-torch", "on", "50", "9999999999999s"}, &args));
  return 0;
}
