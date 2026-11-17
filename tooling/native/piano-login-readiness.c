// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, Leo Zhang. All rights reserved.
// Read only UI proof, except prepare focuses the already selected account's
// secure field. No password value is read or printed. AX denial fails closed.
#include <ApplicationServices/ApplicationServices.h>
#include <libproc.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static CFTypeRef attr(AXUIElementRef e, CFStringRef key)
{
  CFTypeRef value = NULL;
  if (AXUIElementCopyAttributeValue(e, key, &value) != kAXErrorSuccess) return NULL;
  return value;
}
static int equal(CFTypeRef v, CFStringRef wanted)
{ return v && CFGetTypeID(v) == CFStringGetTypeID() && CFEqual(v, wanted); }
static int secure(AXUIElementRef e)
{
  CFTypeRef v = attr(e, kAXSubroleAttribute);
  int ok = equal(v, kAXSecureTextFieldSubrole);
  if (v) CFRelease(v);
  return ok;
}
static void walk(AXUIElementRef e, CFStringRef shortname, CFStringRef fullname,
                 unsigned depth, unsigned *budget, int *account, AXUIElementRef *field)
{
  if (!*budget || depth > 12) return;
  --*budget;
  if (secure(e)) { if (!*field) *field = (AXUIElementRef)CFRetain(e); return; }
  const CFStringRef keys[] = { kAXTitleAttribute, kAXValueAttribute, kAXDescriptionAttribute };
  for (unsigned n = 0; n < 3; ++n) {
    CFTypeRef v = attr(e, keys[n]);
    if (equal(v, shortname) || equal(v, fullname)) *account = 1;
    if (v) CFRelease(v);
  }
  CFTypeRef children = attr(e, kAXChildrenAttribute);
  if (children && CFGetTypeID(children) == CFArrayGetTypeID())
    for (CFIndex i = 0; i < CFArrayGetCount(children); ++i)
      walk((AXUIElementRef)CFArrayGetValueAtIndex(children, i), shortname, fullname, depth + 1, budget, account, field);
  if (children) CFRelease(children);
}
int main(int argc, char **argv)
{
  if (argc != 3 || (strcmp(argv[1], "prepare") && strcmp(argv[1], "ready") && strcmp(argv[1], "desktop"))) return 2;
  uid_t uid = (uid_t)strtoul(argv[2], NULL, 10);
  struct passwd *p = uid ? getpwuid(uid) : NULL;
  if (!p) return 2;
  if (!strcmp(argv[1], "desktop")) {
    struct stat st;
    if (stat("/dev/console", &st) || st.st_uid != uid) return 1;
    puts("console_user_matches=yes"); return 0;
  }
  alarm(10);
  AXUIElementRef system = AXUIElementCreateSystemWide();
  AXUIElementSetMessagingTimeout(system, 2);
  AXUIElementRef app = (AXUIElementRef)attr(system, kAXFocusedApplicationAttribute);
  pid_t pid = 0; char name[64] = {};
  if (!app || AXUIElementGetPid(app, &pid) || proc_name(pid, name, sizeof name) <= 0 || strcmp(name, "SecurityAgent")) {
    puts("selected_login_account=no"); return 1;
  }
  CFStringRef shortname = CFStringCreateWithCString(NULL, p->pw_name, kCFStringEncodingUTF8);
  CFStringRef fullname = CFStringCreateWithCString(NULL, p->pw_gecos, kCFStringEncodingUTF8);
  int account = 0; unsigned budget = 500; AXUIElementRef field = NULL;
  walk(app, shortname, fullname, 0, &budget, &account, &field);
  // A user list is not a selected account: require exactly the login password UI.
  if (!account || !field) { puts("selected_login_account=no"); return 1; }
  if (!strcmp(argv[1], "prepare") && AXUIElementSetAttributeValue(field, kAXFocusedAttribute, kCFBooleanTrue)) return 1;
  AXUIElementRef focused = (AXUIElementRef)attr(app, kAXFocusedUIElementAttribute);
  if (!focused || !CFEqual(focused, field) || !secure(focused)) return 1;
  puts("selected_login_account=yes\nfocused_password_field=yes");
  return 0;
}
