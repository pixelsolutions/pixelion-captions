// Guest access and add-on credits: tests against the real shared headers
// (src/shared/guest-api.h, src/shared/addon-api.h). Needs QtCore + QtNetwork
// only; no OBS. Both platform adapters build their guest request bodies through
// these functions, so the golden bodies below hold for Windows and macOS alike.

#include "addon-api.h"
#include "guest-api.h"

#include <QCoreApplication>

#include <cstdio>
#include <cstdlib>

namespace {
int failures = 0;

void check(bool ok, const char *what)
{
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
}

QByteArray bytes(const char *text) { return QByteArray(text); }
} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	using namespace PixelionGuestApi;

	// ---- golden request bodies (guest routes) ----
	const QString visitor = QStringLiteral("123e4567-e89b-42d3-a456-426614174000");
	check(tokenBody(visitor) ==
		      bytes(R"({"client":"obs_plugin","mode":"transcribe","visitor_id":"123e4567-e89b-42d3-a456-426614174000"})"),
	      "golden guest realtime-token body");
	check(usageCheckBody(visitor, QStringLiteral("sess-1"), 0, 60) ==
		      bytes(R"({"from_second":0,"session_id":"sess-1","to_second":60,"visitor_id":"123e4567-e89b-42d3-a456-426614174000"})"),
	      "golden guest usage-check body");
	check(entitlementsUrl(visitor).toString() ==
		      QStringLiteral("https://api.pixelion.ai/guest/entitlements?visitor_id=123e4567-e89b-42d3-a456-426614174000"),
	      "guest entitlements URL");
	check(!jsonPostRequest(QStringLiteral("/guest/realtime-token"), 1000).hasRawHeader("Authorization"),
	      "guest request carries no Authorization header");

	// ---- visitor id ----
	check(isValidVisitorId(newVisitorId()), "generated visitor id is a valid UUID v4");
	check(isValidVisitorId(visitor), "known v4 id valid");
	check(!isValidVisitorId(QStringLiteral("not-a-uuid")), "garbage id invalid");
	check(!isValidVisitorId(QStringLiteral("123e4567-e89b-12d3-a456-426614174000")), "v1 UUID rejected");

	// ---- GET /guest/entitlements parsing ----
	Usage u;
	check(parseEntitlements(200,
				bytes(R"({"plan_id":"guest_v1","tier":"guest","guest_lifetime_seconds":600,"guest_used_seconds":120,"guest_remaining_seconds":480})"),
				&u) &&
		      u.lifetimeSeconds == 600 && u.usedSeconds == 120 && u.remainingSeconds == 480,
	      "200 with used and remaining");
	check(parseEntitlements(200, bytes(R"({"tier":"guest","guest_lifetime_seconds":600})"), &u) && u.lifetimeSeconds == 600 &&
		      u.usedSeconds == -1 && u.remainingSeconds == -1,
	      "200 without used/remaining leaves them unknown, not zero");
	check(parseEntitlements(200, bytes(R"({"tier":"guest","guest_used_seconds":30})"), &u) && u.lifetimeSeconds == -1 &&
		      effectiveLifetimeSeconds(u) == kDefaultLifetimeSeconds,
	      "missing total falls back to 10 minutes");
	check(!parseEntitlements(400, bytes(R"({"code":"INVALID_VISITOR_ID"})"), &u), "400 is unavailable");
	check(!parseEntitlements(500, bytes(R"({"code":"INTERNAL_ERROR"})"), &u), "500 is unavailable");
	check(!parseEntitlements(404, bytes(""), &u), "404 is unavailable");
	check(!parseEntitlements(0, bytes(""), &u), "network error is unavailable");
	check(!parseEntitlements(200, bytes("{not json"), &u), "malformed body is unavailable");
	check(!parseEntitlements(200, bytes("[1,2]"), &u), "non-object body is unavailable");
	check(parseEntitlements(200, bytes(R"({"guest_lifetime_seconds":null,"guest_used_seconds":"x"})"), &u) &&
		      u.lifetimeSeconds == -1 && u.usedSeconds == -1,
	      "null/wrong-typed numbers are unknown");

	// ---- fallback path: failure keeps the last known value, then the default ----
	Usage known;
	known.lifetimeSeconds = 900;
	known.usedSeconds = 300;
	Usage fresh;
	check(!parseEntitlements(500, bytes(""), &fresh), "failed refresh");
	check(effectiveLifetimeSeconds(known) == 900 && effectiveUsedSeconds(known) == 300, "last known value kept");
	check(effectiveLifetimeSeconds(Usage()) == 600, "no value at all shows 10 minutes");
	check(effectiveUsedSeconds(Usage()) == -1, "unknown used is never shown as 0");
	check(offerText(Usage()) == QStringLiteral("Try 10 minutes of free live captions. No account needed."),
	      "default hint text");
	Usage one;
	one.lifetimeSeconds = 60;
	check(offerText(one).contains(QStringLiteral("1 minute ")), "singular minute");
	Usage twenty;
	twenty.lifetimeSeconds = 1200;
	check(offerText(twenty).contains(QStringLiteral("20 minutes")), "server value is shown, not hardcoded");

	// ---- merge / exhausted ----
	Usage merged = merge(known, Usage{-1, -1, 100});
	check(merged.lifetimeSeconds == 900 && merged.remainingSeconds == 100 && merged.usedSeconds == 800,
	      "remaining-only update keeps used consistent");
	check(!exhausted(known), "not exhausted");
	check(exhausted(Usage{600, 600, 0}), "exhausted when remaining 0");
	check(exhausted(Usage{600, 700, -1}), "exhausted when used >= total");
	check(!exhausted(Usage()), "unknown is not exhausted");
	check(errorCode(bytes(R"({"error":"GUEST_LIMIT_REACHED","code":"GUEST_LIMIT_REACHED"})")) ==
		      QStringLiteral("GUEST_LIMIT_REACHED"),
	      "error code from body");

	// ---- add-on credits ----
	using namespace PixelionAddonApi;
	Entitlements e;
	const char *pro = R"({"plan_id":"pro_v1","tier":"pro","subscription_status":"active","addon_eligible":true,
	  "addon":{"price_cents_per_minute":4,"min_minutes":125,"max_minutes":5000,"default_minutes":500,"max_packs":10},
	  "usage":{"minutes_used":10,"minutes_limit":600,"minutes_remaining":595.567},
	  "addons":{"available":true,"held_minutes":100.5,"packs":[
	    {"pack_id":"p1","purchased_minutes":125,"remaining_minutes":85.999,"used_minutes":39.001,"purchased_at":1,"expires_at":4102444800,"expired":false}]}})";
	check(parseEntitlements(200, bytes(pro), &e) && e.known && e.addonEligible && e.tier == QStringLiteral("pro"),
	      "entitlements parse");
	check(priceText(e) == QStringLiteral("from $5.00 for 125 minutes"), "price text comes from the catalog");
	check(formatMinutes(e.minutesRemaining) == QStringLiteral("595.56"), "remaining rounds down, not up");
	check(formatMinutes(125) == QStringLiteral("125") && formatMinutes(12.5) == QStringLiteral("12.5"), "trailing zeros trimmed");
	check(e.packs.size() == 1 && formatMinutes(e.packs[0].remainingMinutes) == QStringLiteral("85.99"), "pack remaining rounds down");
	check(packText(e.packs[0]).contains(QStringLiteral("85.99 left")), "pack row text");
	check(parseEntitlements(200, bytes(R"({"tier":"pro","addons":{"available":false}})"), &e) && !e.addonsAvailable &&
		      e.heldMinutes == 0.0,
	      "unavailable balance is flagged, not zeroed");
	check(parseEntitlements(200, bytes(R"({"tier":"free"})"), &e) && !e.addonEligible && priceText(e).isEmpty(),
	      "free plan: no buy, no price");
	check(!parseEntitlements(500, bytes(""), &e), "entitlements failure");
	check(checkoutBody(QStringLiteral("limit_popup")) == bytes(R"({"medium":"limit_popup","platform":"obs_plugin"})"),
	      "golden checkout body");
	check(checkoutUrl(200, bytes(R"({"checkout_url":"https://pixelion.ai/checkout?x=1","expires_at":1})")) ==
		      QStringLiteral("https://pixelion.ai/checkout?x=1"),
	      "checkout url");
	check(checkoutUrl(200, bytes(R"({"checkout_url":"http://evil"})")).isEmpty(), "non-https checkout url rejected");
	check(checkoutError(403, bytes(R"({"code":"ADDON_FREE_PLAN"})")).action == CheckoutAction::Upgrade, "free plan -> upgrade");
	check(checkoutError(403, bytes(R"({"code":"ADDON_TRIAL"})")).action == CheckoutAction::None, "trial -> none");
	check(checkoutError(403, bytes(R"({"code":"ADDON_NOT_OFFERED"})")).action == CheckoutAction::None, "not offered -> none");
	check(checkoutError(403, bytes(R"({"code":"ADDON_NO_ACTIVE_SUBSCRIPTION"})")).action == CheckoutAction::Account,
	      "inactive -> account page");
	check(checkoutError(500, bytes("")).action == CheckoutAction::Retry, "5xx -> retry");
	check(checkoutError(0, bytes("")).action == CheckoutAction::Retry, "network error -> retry");

	if (failures == 0)
		std::printf("guest/add-on tests passed\n");
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
