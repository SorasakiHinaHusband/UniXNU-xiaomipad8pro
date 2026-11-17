/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. All rights reserved. */
/* Bounded discovery only. No pairing, connection, remote-name request or keys. */
#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>
#import <CoreBluetooth/CoreBluetooth.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

@interface PianoBtProbe : NSObject <CBCentralManagerDelegate>
@property(nonatomic, strong) CBCentralManager *central;
@property(nonatomic, strong) IOBluetoothDeviceInquiry *inquiry;
@property(nonatomic, strong) NSMutableSet<NSUUID *> *leSeen;
@property(nonatomic) NSUInteger leReports;
@property(nonatomic) NSUInteger classicReports;
@property(nonatomic) BOOL leStarted;
@property(nonatomic) BOOL classicStarted;
@property(nonatomic) BOOL classicComplete;
@property(nonatomic) IOReturn classicError;
@end
@implementation PianoBtProbe
- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    printf("piano-bt-path1 probe: le_state=%ld authorization=%ld\n", (long)central.state, (long)CBManager.authorization);
}
- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral
    advertisementData:(NSDictionary<NSString *, id> *)advertisementData RSSI:(NSNumber *)RSSI
{
    (void)central; (void)advertisementData; (void)RSSI;
    self.leReports += 1;
    if (peripheral.identifier) [self.leSeen addObject:peripheral.identifier];
}
- (void)deviceInquiryStarted:(IOBluetoothDeviceInquiry *)sender
{
    (void)sender;
    self.classicStarted = YES;
    puts("piano-bt-path1 probe: classic_inquiry_started=1");
}
- (void)deviceInquiryDeviceFound:(IOBluetoothDeviceInquiry *)sender device:(IOBluetoothDevice *)device
{
    (void)sender; (void)device;
    self.classicReports += 1;
}
- (void)deviceInquiryComplete:(IOBluetoothDeviceInquiry *)sender error:(IOReturn)error aborted:(BOOL)aborted
{
    (void)sender;
    self.classicComplete = YES;
    self.classicError = error;
    printf("piano-bt-path1 probe: classic_complete=1 error=0x%08x aborted=%u\n", error, (unsigned)aborted);
}
@end
static void spin(double seconds)
{
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([until timeIntervalSinceNow] > 0)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
}
static int usage(void)
{
    fputs("usage: piano-bt-probe --status | --scan SECONDS (1..30 per radio)\n", stderr);
    return 2;
}
int main(int argc, const char *argv[])
{
    @autoreleasepool {
        bool scan = argc == 3 && !strcmp(argv[1], "--scan");
        if (!scan && !(argc == 2 && !strcmp(argv[1], "--status"))) return usage();
        char *end = NULL;
        long seconds = scan ? strtol(argv[2], &end, 10) : 0;
        if (scan && (!end || *end || seconds < 1 || seconds > 30)) return usage();
        /* Also bound any synchronous framework initialization waiting for daemon IPC. */
        alarm((unsigned)(2 * seconds + 30));
        setvbuf(stdout, NULL, _IOLBF, 0);
        IOBluetoothHostController *controller = [IOBluetoothHostController defaultController];
        NSInteger power = controller ? controller.powerState : -1;
        printf("piano-bt-path1 probe: default_object=%u power=%ld\n", controller != nil, (long)power);
        if (!scan) return power == 1 ? 0 : 3;
        if (power != 1) {
            puts("piano-bt-path1 scan: complete=1 controller_not_powered=1 classic_reports=0 le_reports=0");
            return 3;
        }
        PianoBtProbe *probe = [PianoBtProbe new];
        probe.leSeen = [NSMutableSet set];
        probe.inquiry = [IOBluetoothDeviceInquiry inquiryWithDelegate:probe];
        probe.inquiry.updateNewDeviceNames = NO;
        probe.inquiry.inquiryLength = (uint8_t)seconds;
        IOReturn started = [probe.inquiry start];
        printf("piano-bt-path1 probe: classic_start=0x%08x window_s=%ld\n", started, seconds);
        /* Do not conclude early: native inquiry completion may be throttled. */
        spin((double)seconds);
        if (started == kIOReturnSuccess && !probe.classicComplete) (void)[probe.inquiry stop];
        spin(0.2);
        CBManagerAuthorization authorization = CBManager.authorization;
        bool leAuthorized = authorization == CBManagerAuthorizationAllowedAlways;
        if (leAuthorized) {
            probe.central = [[CBCentralManager alloc] initWithDelegate:probe queue:nil
                options:@{CBCentralManagerOptionShowPowerAlertKey:@NO}];
            NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
            while (probe.central.state == CBManagerStateUnknown && [deadline timeIntervalSinceNow] > 0) spin(0.05);
            if (probe.central.state == CBManagerStatePoweredOn) {
                [probe.central scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey:@YES}];
                probe.leStarted = YES;
                spin((double)seconds);
                [probe.central stopScan];
            }
        } else {
            /* In particular, do not open an unattended TCC grant dialog. */
            printf("piano-bt-path1 probe: le_authorization=%ld le_skipped_client_policy=1\n", (long)authorization);
        }
        printf("piano-bt-path1 scan: complete=1 classic_started=%u classic_reports=%lu classic_error=0x%08x le_started=%u le_reports=%lu le_unique=%lu client_authorized=%u\n",
            (unsigned)probe.classicStarted, (unsigned long)probe.classicReports, probe.classicError,
            (unsigned)probe.leStarted, (unsigned long)probe.leReports, (unsigned long)probe.leSeen.count, (unsigned)leAuthorized);
        probe.inquiry.delegate = nil;
        probe.central.delegate = nil;
        alarm(0);
        return probe.classicReports || probe.leReports ? 0 : leAuthorized ? 4 : 5;
    }
}
