using System.Text;
using System.Runtime.InteropServices;
using MoexConnector.AlorEngine;

[DllImport("libc", CallingConvention = CallingConvention.Cdecl)]
static extern int setenv(string name, string value, int overwrite);

if (args.Length != 2)
    throw new InvalidOperationException("expected native ABI path and fake runtime fixture root");

var libraryPath = Path.GetFullPath(args[0]);
var fixtureRoot = Path.GetFullPath(args[1]);
var schemeDirectory = Path.Combine(fixtureRoot, "scheme");
var configDirectory = Path.Combine(fixtureRoot, "config");
var fakeLibrary = Directory.GetFiles(Path.Combine(fixtureRoot, "bin"), "libcgate.*").Single();

MoexNativePersistentHostCreateOptions Options() => new()
{
    RuntimeRoot = fixtureRoot,
    LibraryPath = fakeLibrary,
    SchemeDirectory = schemeDirectory,
    ConfigDirectory = configDirectory,
    EnvironmentSettingsVariable = "MOEX_CABI_V3_ENV",
    CredentialsVariable = "MOEX_CABI_V3_CREDENTIALS",
    SoftwareKeyVariable = "MOEX_CABI_V3_KEY",
    BrokerCodeVariable = "MOEX_CABI_V3_BROKER",
    ClientCodeVariable = "MOEX_CABI_V3_CLIENT",
    ExpectedRelease = "SPECTRA93",
    IsinId = 1001,
    SessionId = 321,
    ArmedTestNetwork = true,
    ArmedTestSession = true,
    ArmedTestPlaza2 = true,
    ArmedTestOrderSend = true,
    BaseExtId = 79,
    BaseAddUserId = 701,
    BaseCancelUserId = 702,
    BaseRecoveryUserId = 703,
    BaseRunId = "managed-v3-order",
    JournalRoot = Path.Combine(fixtureRoot, "managed-journals"),
    ReceiptPath = Path.Combine(fixtureRoot, "managed-receipt.json"),
    ProfileId = "managed-v3-profile",
    ProfileFingerprint = new string('e', 64),
    PolicyVersion = "managed-v3-policy"
};

using var library = MoexNativePersistentHostLibrary.Load(libraryPath);
if (library.AbiVersion != 3 || library.Layout.Count != 10)
    throw new InvalidOperationException("V3 version/layout validation failed");

using (var lifetimeLibrary = MoexNativePersistentHostLibrary.Load(libraryPath))
using (var lifetimeHost = lifetimeLibrary.CreateHost(Options()))
{
    var refused = false;
    try { lifetimeLibrary.Dispose(); } catch (InvalidOperationException) { refused = true; }
    if (!refused) throw new InvalidOperationException("V3 library unloaded while a host handle was alive");
    lifetimeHost.Start();
    lifetimeHost.Stop();
}

using var host = library.CreateHost(Options());
host.Start();
MoexNativeInteropV3.NativeSnapshot snapshot = default;
for (var i = 0; i < 30; i++)
{
    host.Poll();
    snapshot = host.GetSnapshot();
    if (snapshot.observation_ready != 0) break;
}
if (snapshot.observation_ready == 0 || snapshot.new_order_allowed == 0 || snapshot.order_epoch_active != 0)
    throw new InvalidOperationException("managed V3 host was not ready");

var sell = new MoexPersistentOrderRequest(2, "103000", "managed-v3-sell");
var sellPlan = host.PlanOrder(sell);
if (sellPlan.ok == 0) throw new InvalidOperationException("managed V3 sell plan failed");
var sellCanonical = host.CopyPlanCanonical();
if (sellCanonical.Length != sellPlan.canonical_size) throw new InvalidOperationException("managed V3 canonical mismatch");
var sellSha = Encoding.ASCII.GetString(sellPlan.plan_sha256!).TrimEnd('\0');

var badQuantityRejected = false;
try { _ = host.PlanOrder(new MoexPersistentOrderRequest(2, "103000", "bad", Quantity: 2)); }
catch (MoexNativeException exception) when (exception.Result == MoexResult.InvalidArgument) { badQuantityRejected = true; }
if (!badQuantityRejected) throw new InvalidOperationException("managed V3 quantity policy was loosened");

host.BeginOrder(sell, sellCanonical, sellSha);
var submitted = host.SubmitOrder();
if (submitted.lifecycle_state != 2 || submitted.ok != 0) throw new InvalidOperationException("managed V3 submit state");
var working = host.PollOrder();
if (working.lifecycle_state != 4 || working.ok != 0) throw new InvalidOperationException("managed V3 Working state");
if (host.PollOrder().lifecycle_state != 4) throw new InvalidOperationException("managed V3 poll auto-cancelled");
if (host.CancelCurrentOrder().lifecycle_state != 7) throw new InvalidOperationException("managed V3 cancel state");
MoexNativeInteropV3.NativeOrderResult terminal = default;
for (var i = 0; i < 5 && terminal.lifecycle_state != 8; i++) terminal = host.PollOrder();
if (terminal.ok == 0 || terminal.lifecycle_state != 8) throw new InvalidOperationException("managed V3 cancellation did not finish");
host.FinishOrderEpoch();

if (setenv("MOEX_FAKE_EXT_ID", "80", 1) != 0 || setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20103", 1) != 0)
    throw new InvalidOperationException("managed test could not update fake epoch environment");
var buy = new MoexPersistentOrderRequest(1, "102250", "managed-v3-buy");
var buyPlan = host.PlanOrder(buy);
var buyCanonical = host.CopyPlanCanonical();
var buySha = Encoding.ASCII.GetString(buyPlan.plan_sha256!).TrimEnd('\0');
if (buyPlan.ok == 0 || buySha == sellSha) throw new InvalidOperationException("managed V3 fresh BUY plan missing");
if (!Encoding.UTF8.GetString(buyCanonical).Contains("\"ext_id\": 80", StringComparison.Ordinal))
    throw new InvalidOperationException("managed V3 second canonical did not advance ext_id");
host.BeginOrder(buy, buyCanonical, buySha);
if (host.SubmitOrder().lifecycle_state != 2) throw new InvalidOperationException("managed V3 BUY submit state");
var buyWorking = host.PollOrder();
for (var i = 0; i < 3 && buyWorking.lifecycle_state == 2; i++) buyWorking = host.PollOrder();
if (buyWorking.lifecycle_state != 4)
{
    var afterBuyPoll = host.GetSnapshot();
    throw new InvalidOperationException($"managed V3 BUY Working state={buyWorking.lifecycle_state} ok={buyWorking.ok} add_present={buyWorking.add_reply.present} add_ok={buyWorking.add_reply.accepted} add_id={buyWorking.add_reply.order_id} message={Encoding.UTF8.GetString(buyWorking.message!).TrimEnd('\0')} snapshot={afterBuyPoll.lifecycle_state} active={afterBuyPoll.order_epoch_active} err={Encoding.UTF8.GetString(afterBuyPoll.last_error!).TrimEnd('\0')}");
}
if (host.CancelCurrentOrder().lifecycle_state != 7) throw new InvalidOperationException("managed V3 BUY cancel state");
terminal = default;
for (var i = 0; i < 5 && terminal.lifecycle_state != 8; i++) terminal = host.PollOrder();
if (terminal.ok == 0 || terminal.lifecycle_state != 8) throw new InvalidOperationException("managed V3 BUY cancellation did not finish");
host.FinishOrderEpoch();
host.Stop();

Console.WriteLine("dotnet ABI V3 smoke passed");
