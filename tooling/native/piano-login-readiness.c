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
                 unsigned depth, unsigned *budget, int *account, AXUIElementRef *field, unsigned *fields, int *chooser)
{
  if (!*budget || depth > 12) return;
  --*budget;
  if (secure(e)) { ++*fields; if (!*field) *field = (AXUIElementRef)CFRetain(e); return; }
  CFTypeRef role = attr(e, kAXRoleAttribute);
  if (equal(role, kAXListRole) || equal(role, kAXTableRole) || equal(role, kAXOutlineRole) || equal(role, kAXRadioGroupRole)) *chooser = 1;
  if (role) CFRelease(role);
  const CFStringRef keys[] = { kAXTitleAttribute, kAXValueAttribute, kAXDescriptionAttribute };
  for (unsigned n = 0; n < 3; ++n) {
    CFTypeRef v = attr(e, keys[n]);
    if (equal(v, shortname) || equal(v, fullname)) *account = 1;
    if (v) CFRelease(v);
  }
  CFTypeRef children = attr(e, kAXChildrenAttribute);
  if (children && CFGetTypeID(children) == CFArrayGetTypeID())
    for (CFIndex i = 0; i < CFArrayGetCount(children); ++i)
      walk((AXUIElementRef)CFArrayGetValueAtIndex(children, i), shortname, fullname, depth + 1, budget, account, field, fields, chooser);
  if (children) CFRelease(children);
}
// Account labels elsewhere in the app, or a chooser showing several accounts,
// cannot identify the account belonging to this password field.
static AXUIElementRef selected_field(AXUIElementRef app, CFStringRef shortname, CFStringRef fullname)
{
  int account = 0, chooser = 0; unsigned budget = 500, fields = 0;
  AXUIElementRef field = NULL;
  walk(app, shortname, fullname, 0, &budget, &account, &field, &fields, &chooser);
  if (!field || fields != 1 || !budget) { if (field) CFRelease(field); return NULL; }
  AXUIElementRef window = (AXUIElementRef)attr(field, kAXWindowAttribute);
  if (!window) { CFRelease(field); return NULL; }
  CFTypeRef role = attr(window, kAXRoleAttribute);
  if (!equal(role, kAXWindowRole)) { if (role) CFRelease(role); CFRelease(window); CFRelease(field); return NULL; }
  CFRelease(role);
  account = chooser = 0; fields = 0; budget = 500; AXUIElementRef window_field = NULL;
  walk(window, shortname, fullname, 0, &budget, &account, &window_field, &fields, &chooser);
  const int ok = account && !chooser && budget && fields == 1 && window_field && CFEqual(field, window_field);
  if (window_field) CFRelease(window_field);
  CFRelease(window);
  if (!ok) { CFRelease(field); return NULL; }
  return field;
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
  AXUIElementRef field = selected_field(app, shortname, fullname);
  if (!field) { puts("selected_login_account=no"); return 1; }
  if (!strcmp(argv[1], "prepare") && AXUIElementSetAttributeValue(field, kAXFocusedAttribute, kCFBooleanTrue)) return 1;
  AXUIElementRef focused = (AXUIElementRef)attr(app, kAXFocusedUIElementAttribute);
  if (!focused || !CFEqual(focused, field) || !secure(focused)) return 1;
  puts("selected_login_account=yes\nfocused_password_field=yes");
  return 0;
}
