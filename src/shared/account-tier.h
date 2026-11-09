#pragma once

// Account tier rules shared by every Pixelion client: which tier the current
// user is in, how close they are to their limit, and which purchase button a
// tier is allowed to show. Pure functions only (no widgets, no network, no
// global state) so tests/account-tier-tests.cpp can cover them; the dock in
// src/plugin-main.cpp is the only caller in this project.
//
// Nothing here hard-codes an entitlement: the tier comes from the plan label in
// GET /me and the `tier` field of GET /me/entitlements, the limits come from
// GET /me/usage (plan) or GET /guest/entitlements (guest).

#include "addon-api.h"

#include <QString>

#include <algorithm>

namespace PixelionAccountTier {

enum class Tier {
	SignedOut, // no account and guest mode not started: sign-in view
	Guest,     // guest mode, no account
	Free,      // signed in, unpaid plan (free or any trial)
	Pro        // signed in, paid plan
};

// Where the user stands against the allowance of the current period.
enum class LimitState {
	Unknown, // no usable usage numbers yet
	BelowLimit,
	NearLimit, // at or past kNearLimitPercent of the limit, limit not reached
	AtLimit
};

// The only purchase buttons any tier may show.
enum class Action { None, CreateAccount, UpgradeToPro, AddCredits };

// Percentage of the limit at which the Free warning appears.
constexpr int kNearLimitPercent = 80;

inline QString normalizePlan(const QString &plan)
{
	QString value = plan.trimmed().toLower();
	value.replace(QLatin1Char('_'), QLatin1Char(' '));
	value.replace(QLatin1Char('-'), QLatin1Char(' '));
	return value.simplified();
}

// A label that does not pay: no plan at all, the free plan, or a trial of any
// kind (a free trial counts as Free). Everything else is a paid plan.
inline bool isUnpaidPlan(const QString &plan)
{
	const QString value = normalizePlan(plan);
	return value.isEmpty() || value == QStringLiteral("free") || value == QStringLiteral("guest") ||
	       value == QStringLiteral("none") || value.startsWith(QStringLiteral("free ")) ||
	       value.contains(QStringLiteral("trial"));
}

// `entitlementTier` is the tier field of GET /me/entitlements and wins when the
// response is known; the GET /me plan label is the fallback.
inline Tier classify(bool signedIn, bool guestActive, const QString &plan, const QString &entitlementTier = QString())
{
	if (!signedIn)
		return guestActive ? Tier::Guest : Tier::SignedOut;
	const QString label = entitlementTier.trimmed().isEmpty() ? plan : entitlementTier;
	return isUnpaidPlan(label) ? Tier::Free : Tier::Pro;
}

// Unit-agnostic: plan usage is passed in minutes, the guest allowance in
// seconds. A missing or non-positive limit is Unknown, never "below limit".
inline LimitState limitState(bool known, bool unlimited, int used, int limit)
{
	if (!known)
		return LimitState::Unknown;
	if (unlimited)
		return LimitState::BelowLimit;
	if (limit <= 0 || used < 0)
		return LimitState::Unknown;
	if (used >= limit)
		return LimitState::AtLimit;
	// Integer form of used / limit >= 80 %.
	return used * 100 >= limit * kNearLimitPercent ? LimitState::NearLimit : LimitState::BelowLimit;
}

inline bool isNearLimit(int used, int limit)
{
	return limitState(true, false, used, limit) == LimitState::NearLimit;
}

inline bool isAtLimit(int used, int limit)
{
	return limitState(true, false, used, limit) == LimitState::AtLimit;
}

// Button shown in the account card while the user is below their limit, and
// kept visible at every state for the tiers that have one. Pro never gets a
// purchase button here: its only purchase button is in the limit message.
inline Action cardAction(Tier tier)
{
	switch (tier) {
	case Tier::Guest:
		return Action::CreateAccount;
	case Tier::Free:
		return Action::UpgradeToPro;
	case Tier::Pro:
	case Tier::SignedOut:
		break;
	}
	return Action::None;
}

// Button shown with the limit message once the allowance is used up.
// `canBuyCredits` is the server's answer (MINUTES_LIMIT can_buy_addons, or
// addon_eligible), not an assumption about the plan.
inline Action limitAction(Tier tier, bool canBuyCredits = true)
{
	switch (tier) {
	case Tier::Guest:
		return Action::CreateAccount;
	case Tier::Free:
		return Action::UpgradeToPro;
	case Tier::Pro:
		return canBuyCredits ? Action::AddCredits : Action::None;
	case Tier::SignedOut:
		break;
	}
	return Action::None;
}

// Badge text in the account header. Signed-out users get no badge.
inline QString tierLabel(Tier tier)
{
	switch (tier) {
	case Tier::Guest:
		return QStringLiteral("Guest");
	case Tier::Free:
		return QStringLiteral("Free");
	case Tier::Pro:
		return QStringLiteral("Pro");
	case Tier::SignedOut:
		break;
	}
	return {};
}

inline QString actionLabel(Action action)
{
	switch (action) {
	case Action::CreateAccount:
		return QStringLiteral("Create account");
	case Action::UpgradeToPro:
		return QStringLiteral("Upgrade to Pro");
	case Action::AddCredits:
		return QStringLiteral("Add credits");
	case Action::None:
		break;
	}
	return {};
}

// Add-on credit balance summed over the packs that are still valid. Expired
// packs stay out of the bar (they are still listed as rows in the card).
struct CreditTotals {
	bool hasCredits = false; // at least one unexpired pack with purchased minutes
	double purchasedMinutes = 0.0;
	double usedMinutes = 0.0;
	double remainingMinutes = 0.0;
};

inline CreditTotals creditTotals(const PixelionAddonApi::Entitlements &entitlements)
{
	CreditTotals totals;
	// An unreadable balance must never be shown as zero.
	if (!entitlements.known || !entitlements.addonsAvailable)
		return totals;
	for (const auto &pack : entitlements.packs) {
		if (pack.expired)
			continue;
		totals.purchasedMinutes += std::max(0.0, pack.purchasedMinutes);
		totals.usedMinutes += std::max(0.0, pack.usedMinutes);
		totals.remainingMinutes += std::max(0.0, pack.remainingMinutes);
	}
	totals.hasCredits = totals.purchasedMinutes > 0.0;
	return totals;
}

// Spec: available minutes = plan minutes + add-on minutes.
inline double availableMinutes(double planRemainingMinutes, double addonRemainingMinutes)
{
	return std::max(0.0, planRemainingMinutes) + std::max(0.0, addonRemainingMinutes);
}

// Non-blocking Free warning shown in the card at kNearLimitPercent.
inline QString nearLimitWarning(int remainingMinutes)
{
	const int left = std::max(0, remainingMinutes);
	return QStringLiteral("You have used %1% of your plan minutes. %2 left this period.")
		.arg(kNearLimitPercent)
		.arg(left == 1 ? QStringLiteral("1 minute") : QStringLiteral("%1 minutes").arg(left));
}

} // namespace PixelionAccountTier
