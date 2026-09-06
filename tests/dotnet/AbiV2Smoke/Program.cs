using System.Text;
using MoexConnector.AlorEngine;

if (args.Length != 2)
{
    throw new InvalidOperationException("expected native ABI path and fake runtime fixture root");
}

var libraryPath = Path.GetFullPath(args[0]);
var fixtureRoot = Path.GetFullPath(args[1]);
var schemeDirectory = Path.Combine(fixtureRoot, "scheme");
var configDirectory = Path.Combine(fixtureRoot, "config");
var fakeLibrary = Directory.GetFiles(Path.Combine(fixtureRoot, "bin"), "libcgate.*").Single();

var options = new MoexNativeHostV2CreateOptions
{
    Purpose = MoexConnectorHostPurposeV2.Qualify,
    RuntimeRoot = fixtureRoot,
    LibraryPath = fakeLibrary,
    SchemeDirectory = schemeDirectory,
    ConfigDirectory = configDirectory,
    EnvironmentSettingsVariable = "MOEX_CABI_V2_ENV",
    CredentialsVariable = "MOEX_CABI_V2_CREDENTIALS",
    SoftwareKeyVariable = "MOEX_CABI_V2_KEY",
    BrokerCodeVariable = "MOEX_CABI_V2_BROKER",
    ClientCodeVariable = "MOEX_CABI_V2_CLIENT",
    ExpectedRelease = "SPECTRA93",
    IsinId = 1001,
    SessionId = 321,
    ArmedTestNetwork = true,
    ArmedTestSession = true,
    ArmedTestPlaza2 = true,
    Price = "103000",
    BaseContractCode = "RTS",
    JournalRoot = Path.Combine(fixtureRoot, "journals"),
    ReceiptPath = Path.Combine(fixtureRoot, "receipt.json"),
    ProfileId = "managed-v2-qualify",
    ProfileFingerprint = new string('e', 64),
    RunId = "managed-v2-qualify"
};

using var library = MoexNativeHostV2Library.Load(libraryPath);
if (library.AbiVersion != 2 || library.Layout.Count != 9)
{
    throw new InvalidOperationException("V2 version/layout validation failed");
}

using (var lifetimeLibrary = MoexNativeHostV2Library.Load(libraryPath))
{
    var lifetimeOptions = new MoexNativeHostV2CreateOptions
    {
        Purpose = MoexConnectorHostPurposeV2.Qualify,
        RuntimeRoot = fixtureRoot,
        LibraryPath = fakeLibrary,
        SchemeDirectory = schemeDirectory,
        ConfigDirectory = configDirectory,
        EnvironmentSettingsVariable = "MOEX_CABI_V2_ENV",
        CredentialsVariable = "MOEX_CABI_V2_CREDENTIALS",
        SoftwareKeyVariable = "MOEX_CABI_V2_KEY",
        BrokerCodeVariable = "MOEX_CABI_V2_BROKER",
        ClientCodeVariable = "MOEX_CABI_V2_CLIENT",
        ExpectedRelease = "SPECTRA93",
        IsinId = 1001,
        SessionId = 321,
        ArmedTestNetwork = true,
        ArmedTestSession = true,
        ArmedTestPlaza2 = true
    };

    using (var lifetimeHost = lifetimeLibrary.CreateHost(lifetimeOptions))
    {
        var refused = false;
        try
        {
            lifetimeLibrary.Dispose();
        }
        catch (InvalidOperationException)
        {
            refused = true;
        }

        if (!refused)
        {
            throw new InvalidOperationException("V2 library unloaded while a host handle was alive");
        }

        lifetimeHost.Start();
        lifetimeHost.Stop();
    }

    lifetimeLibrary.Dispose();
}

using (var qualify = library.CreateHost(options))
{
    qualify.Start();
    MoexNativeInteropV2.NativeSnapshot snapshot = default;
    for (var i = 0; i < 30; i++)
    {
        qualify.Poll();
        snapshot = qualify.GetSnapshot();
        if (snapshot.observation_ready != 0)
        {
            break;
        }
    }

    if (snapshot.observation_ready == 0 || snapshot.cg_pub_msgnew != 0 || snapshot.cg_pub_post != 0)
    {
        throw new InvalidOperationException("managed V2 qualification snapshot was not ready/no-send");
    }

    var planInfo = qualify.GetPlanInfo();
    var canonical = qualify.CopyPlanCanonical();
    if (planInfo.ok == 0 || planInfo.canonical_size != canonical.Length)
    {
        throw new InvalidOperationException("managed V2 exact plan retrieval failed");
    }

    var rejected = qualify.RunOrderTest();
    if (rejected.ok != 0)
    {
        throw new InvalidOperationException("qualify host unexpectedly ran OrderTest");
    }

    qualify.Stop();
}

options = new MoexNativeHostV2CreateOptions
{
    Purpose = MoexConnectorHostPurposeV2.OrderTest,
    RuntimeRoot = fixtureRoot,
    LibraryPath = fakeLibrary,
    SchemeDirectory = schemeDirectory,
    ConfigDirectory = configDirectory,
    EnvironmentSettingsVariable = "MOEX_CABI_V2_ENV",
    CredentialsVariable = "MOEX_CABI_V2_CREDENTIALS",
    SoftwareKeyVariable = "MOEX_CABI_V2_KEY",
    BrokerCodeVariable = "MOEX_CABI_V2_BROKER",
    ClientCodeVariable = "MOEX_CABI_V2_CLIENT",
    ExpectedRelease = "SPECTRA93",
    IsinId = 1001,
    SessionId = 321,
    ArmedTestNetwork = true,
    ArmedTestSession = true,
    ArmedTestPlaza2 = true,
    ArmedTestOrderSend = true,
    Side = 2,
    Price = "103000",
    BaseContractCode = "RTS",
    ExtId = 79,
    AddUserId = 701,
    CancelUserId = 702,
    RecoveryUserId = 703,
    JournalRoot = Path.Combine(fixtureRoot, "order-journals"),
    ReceiptPath = Path.Combine(fixtureRoot, "order-receipt.json"),
    ProfileId = "managed-v2-order",
    ProfileFingerprint = new string('e', 64),
    RunId = "managed-v2-order"
};

var missingPrice = new MoexNativeHostV2CreateOptions
{
    Purpose = MoexConnectorHostPurposeV2.OrderTest,
    RuntimeRoot = fixtureRoot,
    LibraryPath = fakeLibrary,
    SchemeDirectory = schemeDirectory,
    ConfigDirectory = configDirectory,
    EnvironmentSettingsVariable = "MOEX_CABI_V2_ENV",
    CredentialsVariable = "MOEX_CABI_V2_CREDENTIALS",
    SoftwareKeyVariable = "MOEX_CABI_V2_KEY",
    BrokerCodeVariable = "MOEX_CABI_V2_BROKER",
    ClientCodeVariable = "MOEX_CABI_V2_CLIENT",
    ExpectedRelease = "SPECTRA93",
    IsinId = 1001,
    SessionId = 321,
    ArmedTestNetwork = true,
    ArmedTestSession = true,
    ArmedTestPlaza2 = true,
    ArmedTestOrderSend = true,
    Side = 2,
    BaseContractCode = "RTS",
    ExtId = 79,
    AddUserId = 701,
    CancelUserId = 702,
    RecoveryUserId = 703,
    JournalRoot = Path.Combine(fixtureRoot, "missing-price-journals"),
    ReceiptPath = Path.Combine(fixtureRoot, "missing-price-receipt.json"),
    ProfileId = "managed-v2-missing-price",
    ProfileFingerprint = new string('e', 64),
    RunId = "managed-v2-missing-price"
};

var missingPriceRejected = false;
try
{
    library.CreateHost(missingPrice);
}
catch (ArgumentException)
{
    missingPriceRejected = true;
}

if (!missingPriceRejected)
{
    throw new InvalidOperationException("managed OrderTest accepted a missing price");
}

using (var order = library.CreateHost(options))
{
    order.Start();
    MoexNativeInteropV2.NativeSnapshot snapshot = default;
    for (var i = 0; i < 30; i++)
    {
        order.Poll();
        snapshot = order.GetSnapshot();
        if (snapshot.observation_ready != 0)
        {
            break;
        }
    }

    if (snapshot.observation_ready == 0)
    {
        throw new InvalidOperationException("managed V2 OrderTest host was not ready");
    }

    var info = order.GetPlanInfo();
    var canonical = order.CopyPlanCanonical();
    var sha = Encoding.ASCII.GetString(info.plan_sha256!).TrimEnd('\0');
    order.Authorize(canonical, sha);
    var result = order.RunOrderTest();
    if (result.ok == 0 || result.add_submission.post_invoked == 0 || result.cancel_submission.post_invoked == 0)
    {
        throw new InvalidOperationException("managed V2 fake OrderTest did not complete");
    }

    _ = order.Reconcile();
    order.Stop();
}

Console.WriteLine("dotnet ABI V2 smoke passed");
