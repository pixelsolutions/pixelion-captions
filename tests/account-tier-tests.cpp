// Account tier rules: tests against the real shared header
// (src/shared/account-tier.h). Needs QtCore only; no OBS, no network, no
// widgets. Covers tier classification, the 80 % near-limit boundary, which
// purchase button each tier may show at each limit state, and the credit
// totals that feed the second progress bar.

#include "account-tier.h"
#include "billing-portal.h"

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

PixelionAddonApi::Pack pack(double purchased, double used, double remaining, bool expired)
{
	PixelionAddonApi::Pack p;
	p.purchasedMinutes = purchased;
	p.usedMinutes = used;
	p.remainingMinutes = remaining;
	p.expired = expired;
	return p;
}
} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	using namespace PixelionAccountTier;

	// ---- tier classification ----
	check(classify(false, false, QString()) == Tier::SignedOut, "signed out with no guest session");
	check(classify(false, true, QString()) == Tier::Guest, "guest mode, no account");
	check(classify(true, true, QStringLiteral("pro")) == Tier::Pro, "signing in wins over a leftover guest flag");
	check(classify(true, false, QString()) == Tier::Free, "no plan label is Free");
	check(classify(true, false, QStringLiteral("Free")) == Tier::Free, "free plan is Free");
	check(classify(true, false, QStringLiteral("trial")) == Tier::Free, "trial is Free");
	check(classify(true, false, QStringLiteral("Free Trial")) == Tier::Free, "free trial is Free");
	check(classify(true, false, QStringLiteral("pro_trial")) == Tier::Free, "a trial of Pro still counts as Free");
	check(classify(true, false, QStringLiteral("Pro")) == Tier::Pro, "pro plan is Pro");
	check(classify(true, false, QStringLiteral("hero")) == Tier::Pro, "hero plan is Pro");
	check(classify(true, false, QStringLiteral("  PRO  ")) == Tier::Pro, "plan label is trimmed and lowered");
	// The entitlements tier is the server's own answer and wins over the label.
	check(classify(true, false, QStringLiteral("free"), QStringLiteral("pro")) == Tier::Pro,
	      "entitlement tier pro wins over a stale free label");
	check(classify(true, false, QStringLiteral("pro"), QStringLiteral("free")) == Tier::Free,
	      "entitlement tier free wins over a stale pro label");
	check(classify(true, false, QStringLiteral("pro"), QStringLiteral("  ")) == Tier::Pro,
	      "a blank entitlement tier falls back to the plan label");
	check(classify(true, false, QString(), QStringLiteral("hero")) == Tier::Pro, "hero entitlement tier is Pro");

	// ---- 80 % boundary ----
	check(limitState(false, false, 0, 600) == LimitState::Unknown, "no usage yet is Unknown");
	check(limitState(true, true, 10, 0) == LimitState::BelowLimit, "unlimited is never near or at the limit");
	check(limitState(true, false, 10, 0) == LimitState::Unknown, "a zero limit is Unknown, not AtLimit");
	check(limitState(true, false, -1, 600) == LimitState::Unknown, "an unknown used count is Unknown");
	check(limitState(true, false, 0, 600) == LimitState::BelowLimit, "nothing used");
	check(limitState(true, false, 479, 600) == LimitState::BelowLimit, "one minute under 80 %");
	check(limitState(true, false, 480, 600) == LimitState::NearLimit, "exactly 80 % is the warning");
	check(limitState(true, false, 599, 600) == LimitState::NearLimit, "one minute left is still NearLimit");
	check(limitState(true, false, 600, 600) == LimitState::AtLimit, "limit reached");
	check(limitState(true, false, 900, 600) == LimitState::AtLimit, "over the limit is AtLimit");
	check(!isNearLimit(79, 100) && isNearLimit(80, 100), "boundary on a 100 minute plan");
	check(!isNearLimit(7, 10) && isNearLimit(8, 10), "boundary on a 10 minute plan");
	check(!isNearLimit(100, 100) && isAtLimit(100, 100), "at the limit is not 'near' it");
	// Guest allowance: the same function, in seconds.
	check(limitState(true, false, 480, 600) == LimitState::NearLimit, "80 % of a 10 minute guest allowance");
	check(limitState(true, false, 600, 600) == LimitState::AtLimit, "guest allowance used up");

	// ---- card button per tier, at every limit state ----
	check(cardAction(Tier::Guest) == Action::CreateAccount, "guest card shows Create account");
	check(cardAction(Tier::Free) == Action::UpgradeToPro, "free card shows Upgrade to Pro");
	check(cardAction(Tier::Pro) == Action::None, "pro card shows no purchase button");
	check(cardAction(Tier::SignedOut) == Action::None, "the sign-in view has no card button");
	// "ALWAYS visible": the card button does not depend on the limit state.
	const LimitState states[] = {LimitState::Unknown, LimitState::BelowLimit, LimitState::NearLimit,
				     LimitState::AtLimit};
	for (const LimitState state : states) {
		(void)state;
		check(cardAction(Tier::Guest) == Action::CreateAccount, "guest button stays at every state");
		check(cardAction(Tier::Free) == Action::UpgradeToPro, "free button stays at every state");
		check(cardAction(Tier::Pro) == Action::None, "pro never gains a card button");
	}

	// ---- limit message button per tier ----
	check(limitAction(Tier::Guest) == Action::CreateAccount, "guest limit offers Create account");
	check(limitAction(Tier::Free) == Action::UpgradeToPro, "free limit offers Upgrade to Pro");
	check(limitAction(Tier::Free, true) == Action::UpgradeToPro, "free limit never offers credits");
	check(limitAction(Tier::Pro, true) == Action::AddCredits, "pro limit offers Add credits");
	check(limitAction(Tier::Pro, false) == Action::None, "pro limit offers nothing when the server says no");
	check(limitAction(Tier::SignedOut) == Action::None, "signed out has no limit button");

	// ---- labels ----
	check(actionLabel(Action::CreateAccount) == QStringLiteral("Create account"), "create account label");
	check(actionLabel(Action::UpgradeToPro) == QStringLiteral("Upgrade to Pro"), "upgrade label");
	check(actionLabel(Action::AddCredits) == QStringLiteral("Add credits"), "add credits label");
	check(actionLabel(Action::None).isEmpty(), "no action has no label");
	check(tierLabel(Tier::Guest) == QStringLiteral("Guest"), "guest badge");
	check(tierLabel(Tier::Free) == QStringLiteral("Free"), "free badge");
	check(tierLabel(Tier::Pro) == QStringLiteral("Pro"), "pro badge");
	check(tierLabel(Tier::SignedOut).isEmpty(), "signed out has no badge");

	// ---- credit totals (second progress bar) ----
	PixelionAddonApi::Entitlements none;
	check(!creditTotals(none).hasCredits, "unknown entitlements have no credits");
	PixelionAddonApi::Entitlements unavailable;
	unavailable.known = true;
	unavailable.addonsAvailable = false;
	unavailable.packs.append(pack(125, 25, 100, false));
	check(!creditTotals(unavailable).hasCredits && creditTotals(unavailable).remainingMinutes == 0.0,
	      "an unreadable balance reports no credits instead of a wrong number");
	PixelionAddonApi::Entitlements pro;
	pro.known = true;
	pro.addonEligible = true;
	pro.tier = QStringLiteral("pro");
	pro.packs.append(pack(125, 25, 100, false));
	pro.packs.append(pack(500, 100, 400, false));
	pro.packs.append(pack(60, 60, 0, true)); // expired: not in the bar
	const auto totals = creditTotals(pro);
	check(totals.hasCredits, "unexpired packs give credits");
	check(totals.purchasedMinutes == 625.0, "expired packs are left out of the purchased total");
	check(totals.usedMinutes == 125.0, "used minutes summed over unexpired packs");
	check(totals.remainingMinutes == 500.0, "remaining minutes summed over unexpired packs");
	PixelionAddonApi::Entitlements expiredOnly;
	expiredOnly.known = true;
	expiredOnly.packs.append(pack(60, 60, 0, true));
	check(!creditTotals(expiredOnly).hasCredits, "only expired packs means no credit bar");

	// ---- session cap from the plan (GET /me/entitlements limits) ----
	PixelionAddonApi::Entitlements capped;
	check(PixelionAddonApi::parseEntitlements(
		      200, QByteArray(R"({"plan_id":"free_v2","tier":"free","limits":{"session_cap_min":15}})"), &capped) &&
		      capped.sessionCapMinutes == 15,
	      "free_v2 session cap of 15 minutes is read from the plan response");
	PixelionAddonApi::Entitlements uncapped;
	check(PixelionAddonApi::parseEntitlements(200, QByteArray(R"({"plan_id":"pro","tier":"pro"})"), &uncapped) &&
		      uncapped.sessionCapMinutes == 0,
	      "a plan without a reported cap stays at 0 instead of a made-up number");

	// ---- billing portal link (POST /billing/portal) ----
	check(PixelionBillingPortal::portalUrlFromBody(
		      200, QByteArray(R"({"portal_url":"https://billing.stripe.com/p/session/test_abc"})")) ==
		      QStringLiteral("https://billing.stripe.com/p/session/test_abc"),
	      "a billing.stripe.com https portal URL is accepted");
	check(PixelionBillingPortal::portalUrlFromBody(
		      200, QByteArray(R"({"portal_url":"http://billing.stripe.com/p/x"})")).isEmpty(),
	      "a non-https portal URL is rejected");
	check(PixelionBillingPortal::portalUrlFromBody(
		      200, QByteArray(R"({"portal_url":"https://evil.example.com/p/x"})")).isEmpty(),
	      "a portal URL on another host is rejected");
	check(PixelionBillingPortal::portalUrlFromBody(
		      409, QByteArray(R"({"error":"NO_BILLING_ACCOUNT"})")).isEmpty(),
	      "no billing account returns no URL");
	check(PixelionBillingPortal::portalUrlFromBody(200, QByteArray("{}")).isEmpty(), "missing URL is rejected");
	check(PixelionBillingPortal::errorText(409).contains(QStringLiteral("no subscription")),
	      "409 explains there is no subscription yet");

	// ---- available minutes = plan + add-on ----
	check(availableMinutes(120, 500) == 620.0, "available is plan plus add-on minutes");
	check(availableMinutes(0, 500) == 500.0, "no plan minutes left still leaves credits");
	check(availableMinutes(120, 0) == 120.0, "no credits leaves the plan minutes");
	check(availableMinutes(-5, -5) == 0.0, "negative inputs never reduce the total");
	check(availableMinutes(0, 0) == 0.0, "nothing left");

	// ---- near-limit warning copy ----
	check(nearLimitWarning(120).contains(QStringLiteral("80%")), "the warning names the threshold");
	check(nearLimitWarning(120).contains(QStringLiteral("120 minutes")), "the warning names the minutes left");
	check(nearLimitWarning(1).contains(QStringLiteral("1 minute left")), "singular minute");
	check(nearLimitWarning(-3).contains(QStringLiteral("0 minutes")), "a negative remainder reads as zero");

	if (failures == 0)
		std::printf("account tier tests passed\n");
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
