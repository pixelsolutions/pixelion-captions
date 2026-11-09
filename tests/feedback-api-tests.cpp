// Feedback API mapping: tests against the real shared header
// (src/shared/feedback-api.h). Needs QtCore only. Checks that every category the
// feedback dialog offers maps to a `type` accepted by POST /feedback.

#include "feedback-api.h"

#include <QCoreApplication>

#include <cstdio>

namespace {
int failures = 0;

void expectType(const char *category, const char *expected)
{
	const QString got = feedbackTypeForServer(QString::fromUtf8(category));
	if (got != QString::fromUtf8(expected)) {
		std::fprintf(stderr, "FAIL: category '%s' mapped to '%s', expected '%s'\n", category,
			     got.toUtf8().constData(), expected);
		++failures;
	}
}
} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);

	expectType("bug", "bug");
	expectType("feature", "idea");
	expectType("compliment", "praise");
	expectType("quality", "general");
	expectType("ui", "general");
	expectType("other", "general");
	expectType("", "general");
	expectType("not-a-category", "general");

	if (failures == 0)
		std::printf("feedback-api-tests: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
