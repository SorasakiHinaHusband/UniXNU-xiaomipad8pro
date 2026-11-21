/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. All rights reserved. */
/* The hands-free LE test's peripheral (docs/drivers/bluetooth.md 8.5): runs on a Mac next to the tablet, advertises one GATT service
 * with the test UUID below for a bounded time, answers reads of a 180-byte value that carries a per-run nonce and records writes.
 * No pairing, no encryption requirement, no bonding. Every line goes to stdout (one event per line, stable prefixes for the run's
 * greps). It needs the Mac's Bluetooth permission: run it from a Terminal window (le-peripheral.command), not over ssh.
 *   le-peripheral SECONDS NONCE */
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LE_TEST_SERVICE @"5484D96C-B44C-4777-BFD1-8AFE8D8B5795"
#define LE_TEST_READ @"742C0C8D-28C8-4B48-A51C-1298A09BE548"
#define LE_TEST_WRITE @"E0AFBED1-AD23-4B2A-9ADA-3AD1DB772E09"

static void say(NSString *line)
{
    NSString *stamp = [NSDateFormatter localizedStringFromDate:[NSDate date] dateStyle:NSDateFormatterNoStyle
                                                     timeStyle:NSDateFormatterMediumStyle];
    printf("%s le-peripheral: %s\n", stamp.UTF8String, line.UTF8String);
    fflush(stdout);
}

@interface LePeripheral : NSObject <CBPeripheralManagerDelegate>
@property(nonatomic, strong) CBPeripheralManager *manager;
@property(nonatomic, strong) CBMutableService *service;
@property(nonatomic, strong) NSData *value;
@property(nonatomic) NSUInteger reads, writes;
@property(nonatomic) BOOL added;
@end

@implementation LePeripheral
- (void)peripheralManagerDidUpdateState:(CBPeripheralManager *)peripheral
{
    say([NSString stringWithFormat:@"state=%ld authorization=%ld", (long)peripheral.state, (long)CBManager.authorization]);
    if (peripheral.state != CBManagerStatePoweredOn || self.added) return;
    CBMutableCharacteristic *read = [[CBMutableCharacteristic alloc] initWithType:[CBUUID UUIDWithString:LE_TEST_READ]
        properties:CBCharacteristicPropertyRead value:nil permissions:CBAttributePermissionsReadable];
    CBMutableCharacteristic *write = [[CBMutableCharacteristic alloc] initWithType:[CBUUID UUIDWithString:LE_TEST_WRITE]
        properties:CBCharacteristicPropertyWrite value:nil permissions:CBAttributePermissionsWriteable];
    self.service = [[CBMutableService alloc] initWithType:[CBUUID UUIDWithString:LE_TEST_SERVICE] primary:YES];
    self.service.characteristics = @[read, write];
    self.added = YES;
    [peripheral addService:self.service];
}
- (void)peripheralManager:(CBPeripheralManager *)peripheral didAddService:(CBService *)service error:(NSError *)error
{
    (void)service;
    say([NSString stringWithFormat:@"service_added=%d error=%ld", error == nil, (long)error.code]);
    if (error) return;
    [peripheral startAdvertising:@{CBAdvertisementDataServiceUUIDsKey: @[[CBUUID UUIDWithString:LE_TEST_SERVICE]],
                                   CBAdvertisementDataLocalNameKey: @"PianoLE1"}];
}
- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager *)peripheral error:(NSError *)error
{
    (void)peripheral;
    say([NSString stringWithFormat:@"advertising=%d error=%ld", error == nil, (long)error.code]);
}
- (void)peripheralManager:(CBPeripheralManager *)peripheral didReceiveReadRequest:(CBATTRequest *)request
{
    if (request.offset > self.value.length) {
        [peripheral respondToRequest:request withResult:CBATTErrorInvalidOffset];
        return;
    }
    request.value = [self.value subdataWithRange:NSMakeRange(request.offset, self.value.length - request.offset)];
    [peripheral respondToRequest:request withResult:CBATTErrorSuccess];
    self.reads += 1;
    say([NSString stringWithFormat:@"read offset=%lu reads=%lu mtu_payload=%lu", (unsigned long)request.offset,
        (unsigned long)self.reads, (unsigned long)request.central.maximumUpdateValueLength]);
}
- (void)peripheralManager:(CBPeripheralManager *)peripheral didReceiveWriteRequests:(NSArray<CBATTRequest *> *)requests
{
    for (CBATTRequest *request in requests) {
        self.writes += 1;
        NSString *text = [[NSString alloc] initWithData:request.value encoding:NSUTF8StringEncoding] ?: @"(binary)";
        if (text.length > 64) text = [text substringToIndex:64];
        say([NSString stringWithFormat:@"write bytes=%lu writes=%lu value=%@", (unsigned long)request.value.length,
            (unsigned long)self.writes, text]);
    }
    [peripheral respondToRequest:requests.firstObject withResult:CBATTErrorSuccess];
}
@end

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        if (argc != 3) {
            fputs("usage: le-peripheral SECONDS NONCE (1..3600; NONCE 1..32 characters [A-Za-z0-9-])\n", stderr);
            return 2;
        }
        long seconds = strtol(argv[1], NULL, 10);
        size_t n = strlen(argv[2]);
        if (seconds < 1 || seconds > 3600 || n < 1 || n > 32 || strspn(argv[2], "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-") != n) {
            fputs("le-peripheral: bad arguments\n", stderr);
            return 2;
        }
        LePeripheral *p = [LePeripheral new];
        NSMutableString *text = [NSMutableString stringWithFormat:@"piano-le1 nonce=%s ", argv[2]];
        while (text.length < 180) [text appendString:@"0123456789"];
        p.value = [[text substringToIndex:180] dataUsingEncoding:NSUTF8StringEncoding];
        say([NSString stringWithFormat:@"start seconds=%ld nonce=%s value_bytes=%lu", seconds, argv[2], (unsigned long)p.value.length]);
        p.manager = [[CBPeripheralManager alloc] initWithDelegate:p queue:nil];
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:seconds];
        while ([until timeIntervalSinceNow] > 0)
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
        [p.manager stopAdvertising];
        [p.manager removeAllServices];
        say([NSString stringWithFormat:@"end reads=%lu writes=%lu", (unsigned long)p.reads, (unsigned long)p.writes]);
    }
    return 0;
}
