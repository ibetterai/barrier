/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "MacProximityController.h"

#include "ClientIdentityResolverQueue.h"

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QPointer>

namespace {

// A server advertises this service so clients can pair and gate their
// connection using the server's RSSI.
NSString* const kBarrierServerProximityServiceUuid =
    @"7D2D58C0-8D72-4A4D-9D36-EA6683E7D65B";
NSString* const kBarrierServerIdentityCharacteristicUuid =
    @"F2437B8C-19EC-4E49-A572-7E8823BE1D12";

// A client advertises a distinct service so a server can show that client's
// nearby signal. The readable value is an opaque, pairing-scoped 128-bit
// routing identifier. No name, host, screen, MAC, or account ID is advertised.
NSString* const kBarrierClientPresenceServiceUuid =
    @"30E76136-5481-45B7-9FF5-08C5662C236B";
NSString* const kBarrierClientIdentityCharacteristicUuid =
    @"FC65A88D-D88D-4584-9DD2-DBBF602AE7E3";

enum BarrierAdvertisingRole : NSInteger {
    BarrierAdvertisingRoleNone = 0,
    BarrierAdvertisingRoleServerIdentity = 1,
    BarrierAdvertisingRoleClientPresence = 2
};

enum BarrierClientResolvePhase : NSInteger {
    BarrierClientResolvePhaseIdle = 0,
    BarrierClientResolvePhaseConnecting,
    BarrierClientResolvePhaseDiscoveringServices,
    BarrierClientResolvePhaseDiscoveringCharacteristics,
    BarrierClientResolvePhaseReading
};

QString toQString(NSString* value)
{
    return value == nil ? QString() : QString::fromUtf8([value UTF8String]);
}

NSString* toNSString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return [NSString stringWithUTF8String:utf8.constData()];
}

QUuid peripheralUuid(CBPeripheral* peripheral)
{
    return QUuid(toQString([peripheral.identifier UUIDString]));
}

MacProximityController::BluetoothState bluetoothState(CBManagerState state)
{
    if (@available(macOS 10.15, *)) {
        const CBManagerAuthorization authorization = [CBManager authorization];
        if (authorization == CBManagerAuthorizationDenied ||
            authorization == CBManagerAuthorizationRestricted) {
            return MacProximityController::BluetoothState::Unauthorized;
        }
    }

    switch (state) {
    case CBManagerStateUnknown:
        return MacProximityController::BluetoothState::Unknown;
    case CBManagerStateUnauthorized:
        return MacProximityController::BluetoothState::Unauthorized;
    case CBManagerStatePoweredOff:
        return MacProximityController::BluetoothState::PoweredOff;
    case CBManagerStatePoweredOn:
        return MacProximityController::BluetoothState::PoweredOn;
    case CBManagerStateResetting:
        return MacProximityController::BluetoothState::Unknown;
    case CBManagerStateUnsupported:
        return MacProximityController::BluetoothState::Failed;
    }
    return MacProximityController::BluetoothState::Failed;
}

bool decodeIdentity(const QString& value, QByteArray& identity)
{
    const QByteArray encoded = value.toLatin1();
    identity = QByteArray::fromHex(encoded);
    return encoded.size() == 32 && identity.size() == 16 &&
        QString::fromLatin1(identity.toHex()) == value;
}

QString advertisingOperation(BarrierAdvertisingRole role)
{
    return role == BarrierAdvertisingRoleClientPresence
        ? QStringLiteral("client-presence-advertising")
        : QStringLiteral("advertising");
}

} // namespace

@class BarrierProximityDelegate;

class MacProximityControllerPrivate
{
public:
    explicit MacProximityControllerPrivate(MacProximityController* owner);
    ~MacProximityControllerPrivate();

    void centralStateChanged(CBManagerState state)
    {
        const auto translated = bluetoothState(state);
        const QPointer<MacProximityController> owner(q);
        emit q->centralBluetoothStateChanged(translated);
        // Compatibility for the existing connection-gating integration. This
        // intentionally follows the central manager only.
        if (owner) {
            emit q->bluetoothStateChanged(translated);
        }
    }

    void peripheralStateChanged(CBManagerState state)
    {
        emit q->peripheralBluetoothStateChanged(bluetoothState(state));
    }

    void observed(CBPeripheral* peripheral, NSString* name, int rssi)
    {
        emit q->peripheralObserved(
            peripheralUuid(peripheral), toQString(name), rssi);
    }

    void identityRead(CBPeripheral* peripheral, NSData* identity)
    {
        const QByteArray bytes(static_cast<const char*>(identity.bytes),
                               static_cast<int>(identity.length));
        emit q->pairingIdentityRead(
            peripheralUuid(peripheral), QString::fromLatin1(bytes.toHex()));
    }

    void clientObserved(CBPeripheral* peripheral, int rssi)
    {
        emit q->clientPeripheralObserved(peripheralUuid(peripheral), rssi);
    }

    void clientIdentityRead(const QUuid& peripheralId, NSData* identity)
    {
        const QByteArray bytes(static_cast<const char*>(identity.bytes),
                               static_cast<int>(identity.length));
        emit q->clientPresenceIdentityRead(
            peripheralId, QString::fromLatin1(bytes.toHex()));
    }

    void clientIdentityFailed(
        const QUuid& peripheralId, const QString& message)
    {
        emit q->clientPresenceIdentityFailed(peripheralId, message);
    }

    void clientSessionReset(quint64 generation)
    {
        emit q->clientPresenceScanSessionReset(generation);
    }

    void failed(const QString& operation, const QString& message)
    {
        emit q->operationFailed(operation, message);
    }

    qint64 nowMs() const
    {
        return clock.elapsed();
    }

    MacProximityController* q;
    BarrierProximityDelegate* delegate;
    QElapsedTimer clock;
    barrier::ClientIdentityResolverQueue clientResolver;
};

@interface BarrierProximityDelegate : NSObject
    <CBCentralManagerDelegate, CBPeripheralDelegate,
     CBPeripheralManagerDelegate>
{
    MacProximityControllerPrivate* _bridge;
    CBCentralManager* _central;
    CBPeripheralManager* _peripheralManager;
    NSMutableDictionary* _serverDiscovered;
    NSMutableDictionary* _clientDiscovered;
    CBPeripheral* _pairingPeripheral;
    NSData* _pendingPairingIdentity;
    NSString* _pendingPairingFailure;

    CBPeripheral* _clientResolvePeripheral;
    NSUUID* _clientResolveUuid;
    NSUInteger _clientResolveGeneration;
    BarrierClientResolvePhase _clientResolvePhase;
    NSTimer* _clientResolveTimer;
    NSTimer* _clientQuarantineTimer;

    NSData* _advertisingIdentity;
    BarrierAdvertisingRole _advertisingRole;
    NSUInteger _advertisingGeneration;
    CBMutableService* _pendingAdvertisingService;
    BarrierAdvertisingRole _pendingAdvertisingRole;
    NSUInteger _pendingAdvertisingGeneration;

    BOOL _serverScanRequested;
    BOOL _clientScanRequested;
    NSUInteger _activeScanMask;
}

- (id)initWithBridge:(MacProximityControllerPrivate*)bridge;
- (void)startServerAdvertisingIdentity:(NSData*)identity;
- (void)stopServerAdvertising;
- (void)startClientAdvertisingIdentity:(NSData*)identity;
- (void)stopClientAdvertising;
- (void)startServerScanning;
- (void)stopServerScanning;
- (void)startClientScanning;
- (void)stopClientScanning;
- (void)readIdentityForUuid:(NSUUID*)uuid;
- (void)stopEverything;
- (void)shutdownAndInvalidateBridge;

@end

@implementation BarrierProximityDelegate

- (id)initWithBridge:(MacProximityControllerPrivate*)bridge
{
    self = [super init];
    if (self != nil) {
        _bridge = bridge;
        _serverDiscovered = [[NSMutableDictionary alloc] init];
        _clientDiscovered = [[NSMutableDictionary alloc] init];
        _advertisingRole = BarrierAdvertisingRoleNone;
        _clientResolvePhase = BarrierClientResolvePhaseIdle;
    }
    return self;
}

- (void)dealloc
{
    [self shutdownAndInvalidateBridge];
    [_central release];
    [_peripheralManager release];
    [_serverDiscovered release];
    [_clientDiscovered release];
    [_pendingPairingIdentity release];
    [_pendingPairingFailure release];
    [_clientResolvePeripheral release];
    [_clientResolveUuid release];
    [_advertisingIdentity release];
    [_pendingAdvertisingService release];
    [super dealloc];
}

- (void)shutdownAndInvalidateBridge
{
    if (_bridge == nullptr) {
        return;
    }

    // Qt signals emitted below a temporary self-retain may synchronously
    // destroy the controller. Shut down while the C++ bridge is still valid,
    // then detach native callbacks and make a later deferred dealloc a no-op.
    [self stopEverything];
    _central.delegate = nil;
    _peripheralManager.delegate = nil;
    _bridge = nullptr;
}

- (CBUUID*)serverServiceUuid
{
    return [CBUUID UUIDWithString:kBarrierServerProximityServiceUuid];
}

- (CBUUID*)serverIdentityUuid
{
    return [CBUUID UUIDWithString:kBarrierServerIdentityCharacteristicUuid];
}

- (CBUUID*)clientServiceUuid
{
    return [CBUUID UUIDWithString:kBarrierClientPresenceServiceUuid];
}

- (CBUUID*)clientIdentityUuid
{
    return [CBUUID UUIDWithString:kBarrierClientIdentityCharacteristicUuid];
}

- (CBUUID*)serviceUuidForAdvertisingRole:(BarrierAdvertisingRole)role
{
    return role == BarrierAdvertisingRoleClientPresence
        ? [self clientServiceUuid]
        : [self serverServiceUuid];
}

- (CBUUID*)identityUuidForAdvertisingRole:(BarrierAdvertisingRole)role
{
    return role == BarrierAdvertisingRoleClientPresence
        ? [self clientIdentityUuid]
        : [self serverIdentityUuid];
}

- (void)ensureCentral
{
    if (_central == nil) {
        _central = [[CBCentralManager alloc]
            initWithDelegate:self queue:dispatch_get_main_queue()];
    }
}

- (void)ensurePeripheralManager
{
    if (_peripheralManager == nil) {
        _peripheralManager = [[CBPeripheralManager alloc]
            initWithDelegate:self queue:dispatch_get_main_queue()];
    }
}

- (void)discardPeripheralManager
{
    if (_peripheralManager != nil) {
        _peripheralManager.delegate = nil;
        [_peripheralManager stopAdvertising];
        [_peripheralManager removeAllServices];
        [_peripheralManager release];
        _peripheralManager = nil;
    }
    [self clearPendingAdvertisingService];
}

- (void)clearPendingAdvertisingService
{
    [_pendingAdvertisingService release];
    _pendingAdvertisingService = nil;
    _pendingAdvertisingRole = BarrierAdvertisingRoleNone;
    _pendingAdvertisingGeneration = 0;
}

- (void)configureAdvertisingService
{
    if (_advertisingRole == BarrierAdvertisingRoleNone ||
        _advertisingIdentity == nil ||
        _peripheralManager.state != CBManagerStatePoweredOn) {
        return;
    }
    if (_pendingAdvertisingService != nil &&
        _pendingAdvertisingGeneration == _advertisingGeneration &&
        _pendingAdvertisingRole == _advertisingRole) {
        return;
    }

    [_peripheralManager stopAdvertising];
    [_peripheralManager removeAllServices];
    [self clearPendingAdvertisingService];

    CBMutableCharacteristic* characteristic =
        [[[CBMutableCharacteristic alloc]
            initWithType:[self identityUuidForAdvertisingRole:_advertisingRole]
            properties:CBCharacteristicPropertyRead
            value:nil
            permissions:CBAttributePermissionsReadable] autorelease];
    _pendingAdvertisingService = [[CBMutableService alloc]
        initWithType:[self serviceUuidForAdvertisingRole:_advertisingRole]
        primary:YES];
    _pendingAdvertisingService.characteristics = @[characteristic];
    _pendingAdvertisingRole = _advertisingRole;
    _pendingAdvertisingGeneration = _advertisingGeneration;
    [_peripheralManager addService:_pendingAdvertisingService];
}

- (void)startAdvertisingIdentity:(NSData*)identity
    role:(BarrierAdvertisingRole)role
{
    if (_advertisingRole == role &&
        [_advertisingIdentity isEqualToData:identity]) {
        return;
    }

    ++_advertisingGeneration;
    _advertisingRole = role;
    [_advertisingIdentity release];
    _advertisingIdentity = [identity copy];
    // A manager belongs to exactly one advertising generation. Discarding it
    // makes every late service/start callback unambiguously stale.
    [self discardPeripheralManager];
    [self ensurePeripheralManager];
    [self configureAdvertisingService];
}

- (void)stopAdvertisingRole:(BarrierAdvertisingRole)role
{
    if (_advertisingRole != role) {
        return;
    }

    ++_advertisingGeneration;
    _advertisingRole = BarrierAdvertisingRoleNone;
    [self discardPeripheralManager];
    [_advertisingIdentity release];
    _advertisingIdentity = nil;
}

- (void)startServerAdvertisingIdentity:(NSData*)identity
{
    [self startAdvertisingIdentity:identity
                              role:BarrierAdvertisingRoleServerIdentity];
}

- (void)stopServerAdvertising
{
    [self stopAdvertisingRole:BarrierAdvertisingRoleServerIdentity];
}

- (void)startClientAdvertisingIdentity:(NSData*)identity
{
    [self startAdvertisingIdentity:identity
                              role:BarrierAdvertisingRoleClientPresence];
}

- (void)stopClientAdvertising
{
    [self stopAdvertisingRole:BarrierAdvertisingRoleClientPresence];
}

- (void)stopAllAdvertising
{
    if (_advertisingRole == BarrierAdvertisingRoleNone) {
        return;
    }
    [self stopAdvertisingRole:_advertisingRole];
}

- (void)updateScan
{
    if (_central == nil || _central.state != CBManagerStatePoweredOn) {
        _activeScanMask = 0;
        return;
    }

    const NSUInteger requestedMask =
        (_serverScanRequested ? 1u : 0u) |
        (_clientScanRequested ? 2u : 0u);
    if (requestedMask == _activeScanMask &&
        (requestedMask == 0 || _central.isScanning)) {
        return;
    }

    if (_central.isScanning) {
        [_central stopScan];
    }
    _activeScanMask = requestedMask;
    if (requestedMask == 0) {
        return;
    }

    NSMutableArray* services = [NSMutableArray arrayWithCapacity:2];
    if (_serverScanRequested) {
        [services addObject:[self serverServiceUuid]];
    }
    if (_clientScanRequested) {
        [services addObject:[self clientServiceUuid]];
    }
    [_central scanForPeripheralsWithServices:services
        options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @YES}];
}

- (void)startServerScanning
{
    if (!_serverScanRequested && _pairingPeripheral == nil) {
        [_serverDiscovered removeAllObjects];
    }
    _serverScanRequested = YES;
    [self ensureCentral];
    [self updateScan];
}

- (void)stopServerScanning
{
    _serverScanRequested = NO;
    [self updateScan];
}

- (void)invalidateClientResolveTimer
{
    if (_clientResolveTimer != nil) {
        [_clientResolveTimer invalidate];
        [_clientResolveTimer release];
        _clientResolveTimer = nil;
    }
}

- (void)invalidateClientQuarantineTimer
{
    if (_clientQuarantineTimer != nil) {
        [_clientQuarantineTimer invalidate];
        [_clientQuarantineTimer release];
        _clientQuarantineTimer = nil;
    }
}

- (void)scheduleClientQuarantineTimer
{
    [self invalidateClientQuarantineTimer];
    const qint64 deadline =
        _bridge->clientResolver.nextCentralRetirementDeadlineMs();
    if (deadline < 0) {
        return;
    }

    const qint64 remainingMs = deadline - _bridge->nowMs();
    const NSTimeInterval delay =
        static_cast<NSTimeInterval>(qMax<qint64>(1, remainingMs)) / 1000.0;
    _clientQuarantineTimer = [[NSTimer timerWithTimeInterval:delay
        target:self
        selector:@selector(clientQuarantineTimedOut:)
        userInfo:nil
        repeats:NO] retain];
    [[NSRunLoop mainRunLoop] addTimer:_clientQuarantineTimer
                              forMode:NSRunLoopCommonModes];
}

- (void)clearClientResolveStateAndCancel:(BOOL)cancel
{
    [self invalidateClientResolveTimer];
    CBPeripheral* peripheral = [_clientResolvePeripheral retain];
    [_clientResolvePeripheral release];
    _clientResolvePeripheral = nil;
    [_clientResolveUuid release];
    _clientResolveUuid = nil;
    _clientResolveGeneration = 0;
    _clientResolvePhase = BarrierClientResolvePhaseIdle;
    peripheral.delegate = nil;
    if (cancel && peripheral != nil && _central != nil) {
        _bridge->clientResolver.quarantinePeripheral(
            peripheralUuid(peripheral), _bridge->nowMs());
        [_central cancelPeripheralConnection:peripheral];
        [self scheduleClientQuarantineTimer];
    }
    [peripheral release];
}

- (void)retireCentralForClientQuarantine
{
    if (!_bridge->clientResolver.centralRetirementDue(_bridge->nowMs())) {
        [self scheduleClientQuarantineTimer];
        return;
    }

    const barrier::ClientIdentityResolveRequest interrupted =
        _bridge->clientResolver.activeRequest();
    const bool pairingInterrupted = _pairingPeripheral != nil;
    CBPeripheral* clientPeripheral = [_clientResolvePeripheral retain];
    CBPeripheral* pairingPeripheral = [_pairingPeripheral retain];

    [self invalidateClientQuarantineTimer];
    [self clearClientResolveStateAndCancel:NO];
    pairingPeripheral.delegate = nil;
    _pairingPeripheral = nil;
    [_pendingPairingIdentity release];
    _pendingPairingIdentity = nil;
    [_pendingPairingFailure release];
    _pendingPairingFailure = nil;

    // A timed-out cancellation gives us no terminal callback to establish a
    // safe reuse boundary. Retire the entire central manager generation and
    // detach every delegate before allowing any quarantined UUID back in.
    CBCentralManager* retiredCentral = _central;
    _central = nil;
    _activeScanMask = 0;
    if (retiredCentral != nil) {
        retiredCentral.delegate = nil;
        if (retiredCentral.isScanning) {
            [retiredCentral stopScan];
        }
        if (clientPeripheral != nil) {
            [retiredCentral cancelPeripheralConnection:clientPeripheral];
        }
        if (pairingPeripheral != nil) {
            [retiredCentral cancelPeripheralConnection:pairingPeripheral];
        }
        [retiredCentral release];
    }
    [clientPeripheral release];
    [pairingPeripheral release];
    [_serverDiscovered removeAllObjects];
    [_clientDiscovered removeAllObjects];

    _bridge->clientResolver.centralRetired();
    quint64 resetGeneration = 0;
    if (_bridge->clientResolver.isRunning()) {
        resetGeneration = _bridge->clientResolver.resetSession();
    }
    if (_serverScanRequested || _clientScanRequested) {
        [self ensureCentral];
    }

    const QPointer<MacProximityController> owner(_bridge->q);
    [self retain];
    if (resetGeneration != 0) {
        _bridge->clientSessionReset(resetGeneration);
    }
    if (owner && interrupted.isValid()) {
        _bridge->clientIdentityFailed(
            interrupted.peripheralId,
            QStringLiteral("Barrier restarted Bluetooth after a canceled client-presence connection did not finish."));
    }
    if (owner && pairingInterrupted) {
        _bridge->failed(
            QStringLiteral("pairing"),
            QStringLiteral("Barrier restarted Bluetooth while a pairing read was in progress. Try pairing again."));
    }
    [self release];
}

- (void)clientQuarantineTimedOut:(NSTimer*)timer
{
    (void)timer;
    [self invalidateClientQuarantineTimer];
    [self retireCentralForClientQuarantine];
}

- (BOOL)retireQuarantinedClientPeripheral:(CBPeripheral*)peripheral
{
    if (peripheral == nil ||
        !_bridge->clientResolver.peripheralRetired(
            peripheralUuid(peripheral))) {
        return NO;
    }
    peripheral.delegate = nil;
    [self scheduleClientQuarantineTimer];
    return YES;
}

- (void)startClientScanning
{
    if (_clientScanRequested) {
        return;
    }
    _clientScanRequested = YES;
    [_clientDiscovered removeAllObjects];
    const quint64 generation = _bridge->clientResolver.startSession();
    [self ensureCentral];
    [self updateScan];
    [self retain];
    _bridge->clientSessionReset(generation);
    [self release];
}

- (void)stopClientScanning
{
    if (!_clientScanRequested && !_bridge->clientResolver.isRunning()) {
        return;
    }
    _clientScanRequested = NO;
    [self clearClientResolveStateAndCancel:YES];
    _bridge->clientResolver.stopSession();
    [_clientDiscovered removeAllObjects];
    [self updateScan];
}

- (BOOL)clientRequestMatchesPeripheral:(CBPeripheral*)peripheral
{
    const auto active = _bridge->clientResolver.activeRequest();
    return _clientResolvePeripheral == peripheral &&
        _clientResolveUuid != nil && active.isValid() &&
        active.generation == _clientResolveGeneration &&
        active.peripheralId == peripheralUuid(peripheral) &&
        [_clientResolveUuid isEqual:peripheral.identifier];
}

- (void)scheduleClientResolveTimer
{
    [self invalidateClientResolveTimer];
    const qint64 remainingMs =
        _bridge->clientResolver.activeDeadlineMs() - _bridge->nowMs();
    const NSTimeInterval delay =
        static_cast<NSTimeInterval>(qMax<qint64>(1, remainingMs)) / 1000.0;
    NSDictionary* token = @{
        @"generation": @(_clientResolveGeneration),
        @"peripheral": [_clientResolveUuid UUIDString]
    };
    _clientResolveTimer = [[NSTimer timerWithTimeInterval:delay
        target:self
        selector:@selector(clientResolveTimedOut:)
        userInfo:token
        repeats:NO] retain];
    [[NSRunLoop mainRunLoop] addTimer:_clientResolveTimer
                              forMode:NSRunLoopCommonModes];
}

- (void)advanceClientResolution
{
    if (!_clientScanRequested ||
        _central.state != CBManagerStatePoweredOn ||
        _clientResolvePeripheral != nil ||
        _pairingPeripheral != nil) {
        return;
    }

    while (true) {
        const auto request =
            _bridge->clientResolver.takeNext(_bridge->nowMs());
        if (!request.isValid()) {
            return;
        }

        NSUUID* uuid = [[[NSUUID alloc]
            initWithUUIDString:toNSString(
                request.peripheralId.toString(QUuid::WithoutBraces))]
            autorelease];
        CBPeripheral* peripheral = [_clientDiscovered objectForKey:uuid];
        if (peripheral == nil) {
            _bridge->clientResolver.completeFailure(
                request, _bridge->nowMs());
            continue;
        }

        _clientResolvePeripheral = [peripheral retain];
        _clientResolveUuid = [peripheral.identifier retain];
        _clientResolveGeneration =
            static_cast<NSUInteger>(request.generation);
        _clientResolvePhase = BarrierClientResolvePhaseConnecting;
        peripheral.delegate = self;
        [self scheduleClientResolveTimer];
        [_central connectPeripheral:peripheral options:nil];
        return;
    }
}

- (void)finishClientResolution:(barrier::ClientIdentityResolveRequest)request
    identity:(NSData*)identity
    failure:(NSString*)failure
    schedulerAlreadyCompleted:(BOOL)schedulerAlreadyCompleted
    cancelConnection:(BOOL)cancelConnection
{
    if (_clientResolvePeripheral == nil ||
        request.generation != _clientResolveGeneration ||
        request.peripheralId != peripheralUuid(_clientResolvePeripheral)) {
        return;
    }

    BOOL accepted = schedulerAlreadyCompleted;
    if (!accepted) {
        accepted = identity != nil
            ? _bridge->clientResolver.completeSuccess(request)
            : _bridge->clientResolver.completeFailure(
                  request, _bridge->nowMs());
    }
    if (!accepted) {
        return;
    }

    NSData* retainedIdentity = [identity retain];
    NSString* retainedFailure = [failure retain];
    const QUuid peripheralId = request.peripheralId;
    [_clientDiscovered removeObjectForKey:_clientResolveUuid];
    [self retain];
    [self clearClientResolveStateAndCancel:cancelConnection];
    [self advanceClientResolution];

    if (retainedIdentity != nil) {
        _bridge->clientIdentityRead(peripheralId, retainedIdentity);
    }
    else {
        _bridge->clientIdentityFailed(
            peripheralId, toQString(retainedFailure));
    }
    [retainedIdentity release];
    [retainedFailure release];
    [self release];
}

- (void)failActiveClientResolution:(NSString*)message
{
    const auto request = _bridge->clientResolver.activeRequest();
    [self finishClientResolution:request
                        identity:nil
                         failure:message
       schedulerAlreadyCompleted:NO
                cancelConnection:YES];
}

- (void)failTerminalClientResolution:(NSString*)message
{
    const auto request = _bridge->clientResolver.activeRequest();
    [self finishClientResolution:request
                        identity:nil
                         failure:message
       schedulerAlreadyCompleted:NO
                cancelConnection:NO];
}

- (void)clientResolveTimedOut:(NSTimer*)timer
{
    NSDictionary* token = timer.userInfo;
    if (_clientResolveUuid == nil ||
        [token[@"generation"] unsignedIntegerValue] !=
            _clientResolveGeneration ||
        ![token[@"peripheral"] isEqualToString:
            [_clientResolveUuid UUIDString]]) {
        return;
    }

    barrier::ClientIdentityResolveRequest expired;
    if (!_bridge->clientResolver.expireActive(
            _bridge->nowMs(), &expired)) {
        [self scheduleClientResolveTimer];
        return;
    }
    [self finishClientResolution:expired
                        identity:nil
                         failure:@"Barrier timed out while reading this nearby client's identity."
       schedulerAlreadyCompleted:YES
                cancelConnection:YES];
}

- (void)readIdentityForUuid:(NSUUID*)uuid
{
    [self ensureCentral];
    if (_central.state != CBManagerStatePoweredOn) {
        [self retain];
        _bridge->failed(QStringLiteral("pairing"),
                        QStringLiteral("Bluetooth must be on before pairing a nearby server."));
        [self release];
        return;
    }
    const QUuid peripheralId(toQString([uuid UUIDString]));
    if (_bridge->clientResolver.isQuarantined(peripheralId)) {
        [self retain];
        _bridge->failed(
            QStringLiteral("pairing"),
            QStringLiteral("Barrier is finishing a previous Bluetooth connection to this device. Try pairing again shortly."));
        [self release];
        return;
    }
    if (_pairingPeripheral != nil) {
        [self retain];
        _bridge->failed(QStringLiteral("pairing"),
                        QStringLiteral("A Bluetooth pairing read is already in progress."));
        [self release];
        return;
    }
    CBPeripheral* peripheral = [_serverDiscovered objectForKey:uuid];
    if (peripheral == nil) {
        [self retain];
        _bridge->failed(QStringLiteral("pairing"),
                        QStringLiteral("The selected nearby server is no longer available."));
        [self release];
        return;
    }

    [_pendingPairingIdentity release];
    _pendingPairingIdentity = nil;
    _pairingPeripheral = peripheral;
    [_pendingPairingFailure release];
    _pendingPairingFailure = nil;
    _pairingPeripheral.delegate = self;
    [_central connectPeripheral:_pairingPeripheral options:nil];
}

- (void)emitPairingFailure:(NSString*)message
{
    _pairingPeripheral = nil;
    [_pendingPairingIdentity release];
    _pendingPairingIdentity = nil;
    [_pendingPairingFailure release];
    _pendingPairingFailure = nil;
    if (!_serverScanRequested) {
        [_serverDiscovered removeAllObjects];
    }
    [self advanceClientResolution];
    [self retain];
    _bridge->failed(QStringLiteral("pairing"), toQString(message));
    [self release];
}

- (void)finishPairingFailure:(NSString*)message
{
    if (_pairingPeripheral == nil) {
        return;
    }
    [_pendingPairingIdentity release];
    _pendingPairingIdentity = nil;
    [_pendingPairingFailure release];
    _pendingPairingFailure = [message copy];
    [_central cancelPeripheralConnection:_pairingPeripheral];
}

- (void)stopEverything
{
    [self stopServerScanning];
    [self stopClientScanning];
    [self invalidateClientQuarantineTimer];
    _bridge->clientResolver.centralRetired();
    [self stopAllAdvertising];
    if (_pairingPeripheral != nil) {
        _pairingPeripheral.delegate = nil;
        [_central cancelPeripheralConnection:_pairingPeripheral];
        _pairingPeripheral = nil;
    }
    [_pendingPairingIdentity release];
    _pendingPairingIdentity = nil;
    [_pendingPairingFailure release];
    _pendingPairingFailure = nil;
    [_serverDiscovered removeAllObjects];
    [_clientDiscovered removeAllObjects];
}

- (void)centralManagerDidUpdateState:(CBCentralManager*)central
{
    if (central != _central) {
        return;
    }
    barrier::ClientIdentityResolveRequest interrupted;
    quint64 resetGeneration = 0;
    if (central.state == CBManagerStatePoweredOn) {
        [self updateScan];
        [self advanceClientResolution];
    }
    else {
        _activeScanMask = 0;
        if (_clientScanRequested) {
            interrupted = _bridge->clientResolver.activeRequest();
            [self clearClientResolveStateAndCancel:YES];
            resetGeneration = _bridge->clientResolver.resetSession();
            [_clientDiscovered removeAllObjects];
        }
    }

    const QPointer<MacProximityController> owner(_bridge->q);
    [self retain];
    _bridge->centralStateChanged(central.state);
    if (owner && resetGeneration != 0) {
        _bridge->clientSessionReset(resetGeneration);
    }
    if (owner && interrupted.isValid()) {
        _bridge->clientIdentityFailed(
            interrupted.peripheralId,
            QStringLiteral("Bluetooth became unavailable while reading this nearby client's identity."));
    }
    [self release];
}

- (void)failAdvertisingAttempt:(BarrierAdvertisingRole)role
    message:(const QString&)message
{
    if (role == BarrierAdvertisingRoleNone || role != _advertisingRole) {
        return;
    }

    const QString operation = advertisingOperation(role);
    ++_advertisingGeneration;
    _advertisingRole = BarrierAdvertisingRoleNone;
    [self discardPeripheralManager];
    [_advertisingIdentity release];
    _advertisingIdentity = nil;

    // Clear internal state before notifying the GUI. A subsequent request for
    // the same role/identity must create a fresh manager and genuinely retry.
    [self retain];
    _bridge->failed(operation, message);
    [self release];
}

- (void)peripheralManagerDidUpdateState:(CBPeripheralManager*)peripheral
{
    if (peripheral != _peripheralManager) {
        return;
    }
    if (peripheral.state == CBManagerStatePoweredOn) {
        [self configureAdvertisingService];
    }
    else {
        [self clearPendingAdvertisingService];
    }
    [self retain];
    _bridge->peripheralStateChanged(peripheral.state);
    [self release];
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral
    didAddService:(CBService*)service error:(NSError*)error
{
    if (peripheral != _peripheralManager ||
        service != _pendingAdvertisingService ||
        _pendingAdvertisingGeneration != _advertisingGeneration ||
        _pendingAdvertisingRole != _advertisingRole ||
        _advertisingRole == BarrierAdvertisingRoleNone) {
        return;
    }

    const BarrierAdvertisingRole callbackRole = _pendingAdvertisingRole;
    if (error != nil) {
        [self failAdvertisingAttempt:
            callbackRole
            message:
            callbackRole == BarrierAdvertisingRoleClientPresence
                ? QStringLiteral("Barrier could not publish its Bluetooth client-presence service.")
                : QStringLiteral("Barrier could not publish its Bluetooth proximity service.")];
        return;
    }

    [peripheral startAdvertising:@{
        CBAdvertisementDataServiceUUIDsKey:
            @[[self serviceUuidForAdvertisingRole:callbackRole]]
    }];
}

- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager*)peripheral
    error:(NSError*)error
{
    if (peripheral != _peripheralManager ||
        _pendingAdvertisingGeneration != _advertisingGeneration ||
        _pendingAdvertisingRole != _advertisingRole ||
        _advertisingRole == BarrierAdvertisingRoleNone) {
        return;
    }
    const BarrierAdvertisingRole callbackRole = _advertisingRole;
    if (error != nil) {
        [self failAdvertisingAttempt:
            callbackRole
            message:
            callbackRole == BarrierAdvertisingRoleClientPresence
                ? QStringLiteral("Barrier could not start Bluetooth client-presence advertising.")
                : QStringLiteral("Barrier could not start Bluetooth proximity advertising.")];
    }
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral
    didReceiveReadRequest:(CBATTRequest*)request
{
    if (_advertisingRole == BarrierAdvertisingRoleNone ||
        _advertisingIdentity == nil ||
        ![request.characteristic.UUID isEqual:
            [self identityUuidForAdvertisingRole:_advertisingRole]]) {
        [peripheral respondToRequest:request
                         withResult:CBATTErrorAttributeNotFound];
        return;
    }
    if (request.offset > _advertisingIdentity.length) {
        [peripheral respondToRequest:request
                         withResult:CBATTErrorInvalidOffset];
        return;
    }
    request.value = [_advertisingIdentity subdataWithRange:NSMakeRange(
        request.offset, _advertisingIdentity.length - request.offset)];
    [peripheral respondToRequest:request withResult:CBATTErrorSuccess];
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral
    didReceiveWriteRequests:(NSArray<CBATTRequest*>*)requests
{
    if (requests.count != 0) {
        [peripheral respondToRequest:requests[0]
                         withResult:CBATTErrorWriteNotPermitted];
    }
}

- (void)centralManager:(CBCentralManager*)central
    didDiscoverPeripheral:(CBPeripheral*)peripheral
    advertisementData:(NSDictionary<NSString*, id>*)advertisementData
    RSSI:(NSNumber*)RSSI
{
    if (central != _central) {
        return;
    }
    const int rssi = RSSI.intValue;
    if (rssi == 127) {
        return;
    }

    NSArray* advertisedServices =
        advertisementData[CBAdvertisementDataServiceUUIDsKey];
    BOOL isServer = [advertisedServices containsObject:[self serverServiceUuid]];
    BOOL isClient = [advertisedServices containsObject:[self clientServiceUuid]];
    // CoreBluetooth can omit the service list after filtering. Only infer a
    // role when a single role is being scanned, so an opaque client can never
    // leak into the named server-pairing path.
    if (!isServer && !isClient) {
        if (_serverScanRequested && !_clientScanRequested) {
            isServer = YES;
        }
        else if (_clientScanRequested && !_serverScanRequested) {
            isClient = YES;
        }
    }

    if (isServer && _serverScanRequested) {
        [_serverDiscovered setObject:peripheral forKey:peripheral.identifier];
        NSString* name = advertisementData[CBAdvertisementDataLocalNameKey];
        if (name == nil) {
            name = peripheral.name;
        }
        if (name == nil) {
            name = @"Nearby Barrier server";
        }
        const QPointer<MacProximityController> owner(_bridge->q);
        [self retain];
        _bridge->observed(peripheral, name, rssi);
        [self release];
        if (!owner) {
            return;
        }
    }

    if (isClient && _clientScanRequested) {
        const QUuid observedId = peripheralUuid(peripheral);
        const bool admitted = _bridge->clientResolver.enqueue(
            observedId, _bridge->nowMs());
        if (admitted) {
            [_clientDiscovered setObject:peripheral
                                  forKey:peripheral.identifier];
        }
        [self advanceClientResolution];
        // Do not forward rotating/rejected advertiser IDs to the dialog. The
        // resolver's tracked set is session-bounded, so its RSSI maps stay
        // bounded too while resolved clients continue receiving samples.
        if (_bridge->clientResolver.tracks(observedId)) {
            [self retain];
            _bridge->clientObserved(peripheral, rssi);
            [self release];
        }
    }
}

- (void)centralManager:(CBCentralManager*)central
    didConnectPeripheral:(CBPeripheral*)peripheral
{
    if (central != _central) {
        return;
    }
    if (peripheral == _pairingPeripheral) {
        peripheral.delegate = self;
        [peripheral discoverServices:@[[self serverServiceUuid]]];
        return;
    }
    if ([self clientRequestMatchesPeripheral:peripheral] &&
        _clientResolvePhase == BarrierClientResolvePhaseConnecting) {
        _clientResolvePhase = BarrierClientResolvePhaseDiscoveringServices;
        peripheral.delegate = self;
        [peripheral discoverServices:@[[self clientServiceUuid]]];
    }
}

- (void)centralManager:(CBCentralManager*)central
    didFailToConnectPeripheral:(CBPeripheral*)peripheral
    error:(NSError*)error
{
    if (central != _central) {
        return;
    }
    (void)error;
    if ([self retireQuarantinedClientPeripheral:peripheral]) {
        return;
    }
    if (peripheral == _pairingPeripheral) {
        [self emitPairingFailure:
            @"Barrier could not connect to the selected nearby server."];
    }
    else if ([self clientRequestMatchesPeripheral:peripheral]) {
        [self failTerminalClientResolution:
            @"Barrier could not connect to this nearby client over Bluetooth."];
    }
}

- (void)centralManager:(CBCentralManager*)central
    didDisconnectPeripheral:(CBPeripheral*)peripheral
    error:(NSError*)error
{
    if (central != _central) {
        return;
    }
    (void)error;
    if ([self retireQuarantinedClientPeripheral:peripheral]) {
        return;
    }
    if (peripheral == _pairingPeripheral) {
        if (_pendingPairingIdentity != nil) {
            NSData* identity = [[_pendingPairingIdentity retain] autorelease];
            _pairingPeripheral = nil;
            [_pendingPairingIdentity release];
            _pendingPairingIdentity = nil;
            [_pendingPairingFailure release];
            _pendingPairingFailure = nil;
            if (!_serverScanRequested) {
                [_serverDiscovered removeAllObjects];
            }
            [self advanceClientResolution];
            [self retain];
            _bridge->identityRead(peripheral, identity);
            [self release];
            return;
        }
        if (_pendingPairingFailure != nil) {
            NSString* message = [[_pendingPairingFailure retain] autorelease];
            [self emitPairingFailure:message];
            return;
        }
        [self emitPairingFailure:
            @"The Bluetooth pairing connection was interrupted."];
        return;
    }
    if ([self clientRequestMatchesPeripheral:peripheral]) {
        [self failTerminalClientResolution:
            @"The Bluetooth client-presence connection was interrupted."];
    }
}

- (void)peripheral:(CBPeripheral*)peripheral
    didDiscoverServices:(NSError*)error
{
    if (peripheral == _pairingPeripheral) {
        if (error != nil) {
            [self finishPairingFailure:
                @"Barrier could not find its proximity service on the selected server."];
            return;
        }
        for (CBService* service in peripheral.services) {
            if ([service.UUID isEqual:[self serverServiceUuid]]) {
                [peripheral discoverCharacteristics:@[[self serverIdentityUuid]]
                                          forService:service];
                return;
            }
        }
        [self finishPairingFailure:
            @"The selected server does not provide Barrier proximity pairing."];
        return;
    }

    if (![self clientRequestMatchesPeripheral:peripheral] ||
        _clientResolvePhase !=
            BarrierClientResolvePhaseDiscoveringServices) {
        return;
    }
    if (error != nil) {
        [self failActiveClientResolution:
            @"Barrier could not find its presence service on this nearby client."];
        return;
    }
    for (CBService* service in peripheral.services) {
        if ([service.UUID isEqual:[self clientServiceUuid]]) {
            _clientResolvePhase =
                BarrierClientResolvePhaseDiscoveringCharacteristics;
            [peripheral discoverCharacteristics:@[[self clientIdentityUuid]]
                                      forService:service];
            return;
        }
    }
    [self failActiveClientResolution:
        @"This nearby client does not provide Barrier client presence."];
}

- (void)peripheral:(CBPeripheral*)peripheral
    didDiscoverCharacteristicsForService:(CBService*)service
    error:(NSError*)error
{
    if (peripheral == _pairingPeripheral) {
        if (error != nil) {
            [self finishPairingFailure:
                @"Barrier could not read the selected server's pairing identity."];
            return;
        }
        for (CBCharacteristic* characteristic in service.characteristics) {
            if ([characteristic.UUID isEqual:[self serverIdentityUuid]]) {
                [peripheral readValueForCharacteristic:characteristic];
                return;
            }
        }
        [self finishPairingFailure:
            @"The selected server has no Barrier pairing identity."];
        return;
    }

    if (![self clientRequestMatchesPeripheral:peripheral] ||
        _clientResolvePhase !=
            BarrierClientResolvePhaseDiscoveringCharacteristics) {
        return;
    }
    if (error != nil) {
        [self failActiveClientResolution:
            @"Barrier could not read this nearby client's presence identity."];
        return;
    }
    for (CBCharacteristic* characteristic in service.characteristics) {
        if ([characteristic.UUID isEqual:[self clientIdentityUuid]]) {
            _clientResolvePhase = BarrierClientResolvePhaseReading;
            [peripheral readValueForCharacteristic:characteristic];
            return;
        }
    }
    [self failActiveClientResolution:
        @"This nearby client has no Barrier presence identity."];
}

- (void)peripheral:(CBPeripheral*)peripheral
    didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic
    error:(NSError*)error
{
    if (peripheral == _pairingPeripheral) {
        if (error != nil ||
            ![characteristic.UUID isEqual:[self serverIdentityUuid]] ||
            characteristic.value.length != 16) {
            [self finishPairingFailure:
                @"The selected server returned an invalid pairing identity."];
            return;
        }

        [_pendingPairingIdentity release];
        _pendingPairingIdentity = [characteristic.value copy];
        [_central cancelPeripheralConnection:peripheral];
        return;
    }

    if (![self clientRequestMatchesPeripheral:peripheral] ||
        _clientResolvePhase != BarrierClientResolvePhaseReading) {
        return;
    }
    if (error != nil ||
        ![characteristic.UUID isEqual:[self clientIdentityUuid]] ||
        characteristic.value.length != 16) {
        [self failActiveClientResolution:
            @"This nearby client returned an invalid Barrier presence identity."];
        return;
    }

    const auto request = _bridge->clientResolver.activeRequest();
    [self finishClientResolution:request
                        identity:characteristic.value
                         failure:nil
       schedulerAlreadyCompleted:NO
                cancelConnection:YES];
}

@end

MacProximityControllerPrivate::MacProximityControllerPrivate(
    MacProximityController* owner) :
    q(owner),
    delegate([[BarrierProximityDelegate alloc] initWithBridge:this])
{
    clock.start();
}

MacProximityControllerPrivate::~MacProximityControllerPrivate()
{
    [delegate shutdownAndInvalidateBridge];
    [delegate release];
}

MacProximityController::MacProximityController(QObject* parent) :
    QObject(parent),
    m_private(new MacProximityControllerPrivate(this))
{
}

MacProximityController::~MacProximityController()
{
    delete m_private;
}

void MacProximityController::startAdvertising(const QString& proximityId)
{
    QByteArray identity;
    if (!decodeIdentity(proximityId, identity)) {
        emit operationFailed(
            QStringLiteral("advertising"),
            QStringLiteral("The stored Bluetooth proximity identity is invalid."));
        return;
    }
    NSData* data = [NSData dataWithBytes:identity.constData()
                                  length:static_cast<NSUInteger>(identity.size())];
    [m_private->delegate startServerAdvertisingIdentity:data];
}

void MacProximityController::stopAdvertising()
{
    [m_private->delegate stopServerAdvertising];
}

void MacProximityController::startClientPresenceAdvertising(
    const QString& clientProximityId)
{
    QByteArray identity;
    if (!decodeIdentity(clientProximityId, identity)) {
        emit operationFailed(
            QStringLiteral("client-presence-advertising"),
            QStringLiteral("The stored Bluetooth client-presence identity is invalid."));
        return;
    }
    NSData* data = [NSData dataWithBytes:identity.constData()
                                  length:static_cast<NSUInteger>(identity.size())];
    [m_private->delegate startClientAdvertisingIdentity:data];
}

void MacProximityController::stopClientPresenceAdvertising()
{
    [m_private->delegate stopClientAdvertising];
}

void MacProximityController::startScanning()
{
    ++m_scanRequestCount;
    if (m_scanRequestCount == 1) {
        [m_private->delegate startServerScanning];
    }
}

void MacProximityController::stopScanning()
{
    if (m_scanRequestCount == 0) {
        return;
    }
    --m_scanRequestCount;
    if (m_scanRequestCount == 0) {
        [m_private->delegate stopServerScanning];
    }
}

void MacProximityController::startClientPresenceScanning()
{
    ++m_clientPresenceScanRequestCount;
    if (m_clientPresenceScanRequestCount == 1) {
        [m_private->delegate startClientScanning];
    }
}

void MacProximityController::stopClientPresenceScanning()
{
    if (m_clientPresenceScanRequestCount == 0) {
        return;
    }
    --m_clientPresenceScanRequestCount;
    if (m_clientPresenceScanRequestCount == 0) {
        [m_private->delegate stopClientScanning];
    }
}

void MacProximityController::readPairingIdentity(const QUuid& peripheralId)
{
    if (peripheralId.isNull()) {
        emit operationFailed(
            QStringLiteral("pairing"),
            QStringLiteral("The selected Bluetooth peripheral is invalid."));
        return;
    }
    NSUUID* uuid = [[[NSUUID alloc]
        initWithUUIDString:toNSString(
            peripheralId.toString(QUuid::WithoutBraces))] autorelease];
    [m_private->delegate readIdentityForUuid:uuid];
}
