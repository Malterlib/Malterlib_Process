// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Process/ProcessLaunch>

#ifdef DPlatformFamily_Windows
#	include <Windows.h>
#	include <shellapi.h>
#endif

namespace
{

	class CProcessParamEncoding_Tests : public CTest
	{
	public:

		CProcessParamEncoding_Tests()
		{
		}

		// Verify the exact byte-level output of fs_GetParamsWindows. Round-trip
		// alone passes for some incorrect encodings (e.g. anywhere both encoder
		// and parser drop backslashes symmetrically), so we lock down the exact
		// expected output as well.
		static void fs_TestEncoded(std::initializer_list<NMib::NStr::CStr> _Params, NMib::NStr::CStr const &_Expected)
		{
			using namespace NMib;

			NContainer::TCVector<NStr::CStr> Params(_Params);
			NStr::CStr Encoded = NProcess::CProcessLaunchParams::fs_GetParamsWindows(Params);
			DMibTest(DMibExpr(Encoded) == DMibExpr(_Expected));
		}

		// Verify fs_ParseCommandLineWindows produces the exact expected argv tokens
		// (executable + args) for a given cmdline string. Locks down the parser
		// behavior that backs the ParseCommandLineWindows DSL builtin.
		static void fs_TestParse(NMib::NStr::CStr const &_CommandLine, std::initializer_list<NMib::NStr::CStr> _ExpectedTokens)
		{
			using namespace NMib;

			NContainer::TCVector<NStr::CStr> Expected(_ExpectedTokens);

			NStr::CStr Executable;
			auto Args = NProcess::CProcessLaunchParams::fs_ParseCommandLineWindows(_CommandLine, Executable);

			NContainer::TCVector<NStr::CStr> Actual;
			Actual.f_Insert(fg_Move(Executable));
			for (auto &Arg : Args)
				Actual.f_Insert(fg_Move(Arg));

			DMibTest(DMibExpr(Actual.f_GetLen()) == DMibExpr(Expected.f_GetLen()));

			umint nMin = Actual.f_GetLen() < Expected.f_GetLen() ? Actual.f_GetLen() : Expected.f_GetLen();
			for (umint i = 0; i < nMin; ++i)
				DMibTest(DMibExpr(Actual[i]) == DMibExpr(Expected[i]))(ETestFlag_Aggregated);
		}

		// Round-trip: parse a cmd-style cmdline, re-encode with CRT rules, parse
		// the encoded string again, and expect the same tokens. This is the path
		// the ParseCommandLineWindows -> EscapeWindows DSL chain takes.
		static void fs_TestParseEncodeRoundTrip(NMib::NStr::CStr const &_CommandLine)
		{
			using namespace NMib;

			NStr::CStr ExecutableA;
			auto ArgsA = NProcess::CProcessLaunchParams::fs_ParseCommandLineWindows(_CommandLine, ExecutableA);

			NContainer::TCVector<NStr::CStr> TokensA;
			TokensA.f_Insert(fg_Move(ExecutableA));
			for (auto &Arg : ArgsA)
				TokensA.f_Insert(fg_Move(Arg));

			NStr::CStr Encoded = NProcess::CProcessLaunchParams::fs_GetParamsWindows(TokensA);

			NStr::CStr ExecutableB;
			auto ArgsB = NProcess::CProcessLaunchParams::fs_ParseCommandLineWindows(Encoded, ExecutableB);

			NContainer::TCVector<NStr::CStr> TokensB;
			TokensB.f_Insert(fg_Move(ExecutableB));
			for (auto &Arg : ArgsB)
				TokensB.f_Insert(fg_Move(Arg));

			DMibTest(DMibExpr(TokensB.f_GetLen()) == DMibExpr(TokensA.f_GetLen()));

			umint nMin = TokensB.f_GetLen() < TokensA.f_GetLen() ? TokensB.f_GetLen() : TokensA.f_GetLen();
			for (umint i = 0; i < nMin; ++i)
				DMibTest(DMibExpr(TokensB[i]) == DMibExpr(TokensA[i]))(ETestFlag_Aggregated);
		}

		// Round-trip: encode params with fs_GetParamsWindows, then parse back
		// with fs_ParseCommandLineWindows and expect identical args.
		static void fs_TestRoundTrip(std::initializer_list<NMib::NStr::CStr> _Params)
		{
			using namespace NMib;

			NContainer::TCVector<NStr::CStr> Params(_Params);

			// fs_ParseCommandLineWindows expects argv[0] (executable) at the start.
			// Prepend a synthetic exe and re-encode the whole thing the same way
			// CreateProcess sees it.
			NContainer::TCVector<NStr::CStr> WithExe;
			WithExe.f_Insert("dummy.exe");
			for (auto &Param : Params)
				WithExe.f_Insert(Param);

			NStr::CStr Encoded = NProcess::CProcessLaunchParams::fs_GetParamsWindows(WithExe);

			NStr::CStr Executable;
			auto Decoded = NProcess::CProcessLaunchParams::fs_ParseCommandLineWindows(Encoded, Executable);

			DMibTest(DMibExpr(Decoded.f_GetLen()) == DMibExpr(Params.f_GetLen()));

			umint nMin = Decoded.f_GetLen() < Params.f_GetLen() ? Decoded.f_GetLen() : Params.f_GetLen();
			for (umint i = 0; i < nMin; ++i)
				DMibTest(DMibExpr(Decoded[i]) == DMibExpr(Params[i]))(ETestFlag_Aggregated);
		}

#ifdef DPlatformFamily_Windows
		// On Windows additionally verify alignment with CommandLineToArgvW —
		// the same parser CreateProcess'd children use to recover argv.
		static void fs_TestRoundTripCommandLineToArgvW(std::initializer_list<NMib::NStr::CStr> _Params)
		{
			using namespace NMib;

			NContainer::TCVector<NStr::CStr> Params(_Params);

			NContainer::TCVector<NStr::CStr> WithExe;
			WithExe.f_Insert("dummy.exe");
			for (auto &Param : Params)
				WithExe.f_Insert(Param);

			NStr::CStr Encoded = NProcess::CProcessLaunchParams::fs_GetParamsWindows(WithExe);

			// CommandLineToArgvW operates on UTF-16
			NStr::CWStr EncodedW = Encoded;

			int nArgs = 0;
			LPWSTR *pArgs = ::CommandLineToArgvW(reinterpret_cast<LPCWSTR>(EncodedW.f_GetStr()), &nArgs);
			DMibTest(DMibExpr(pArgs != nullptr));
			if (!pArgs)
				return;

			auto Cleanup = g_OnScopeExit / [&]
				{
					::LocalFree(pArgs);
				}
			;

			// argv[0] is the dummy executable; CommandLineToArgvW uses simpler
			// rules for the first token (no backslash escaping), so just skip it
			// and compare the remaining args.
			DMibTest(DMibExpr((umint)nArgs) == DMibExpr(Params.f_GetLen() + 1));

			umint nMin = ((umint)nArgs - 1) < Params.f_GetLen() ? ((umint)nArgs - 1) : Params.f_GetLen();
			for (umint i = 0; i < nMin; ++i)
			{
				NStr::CWStr ArgW = reinterpret_cast<ch16 const *>(pArgs[i + 1]);
				NStr::CStr Arg = ArgW;
				DMibTest(DMibExpr(Arg) == DMibExpr(Params[i]))(ETestFlag_Aggregated);
			}
		}
#endif

		void f_DoTests()
		{
			DMibTestSuite("Parse")
			{
				// Path with no spaces, no quotes — single executable token.
				{
					DMibTestPath("Bare");
					fs_TestParse("c:/Dev/foo.exe", {"c:/Dev/foo.exe"});
				}
				{
					DMibTestPath("BareWithArgs");
					fs_TestParse("c:/Dev/foo.exe --arg1 --arg2", {"c:/Dev/foo.exe", "--arg1", "--arg2"});
				}

				// Path with spaces wrapped in cmd-style quotes — quotes are
				// consumed; the path is one token with the space preserved.
				{
					DMibTestPath("QuotedExe");
					fs_TestParse("\"c:/Program Files/foo.exe\"", {"c:/Program Files/foo.exe"});
				}
				{
					DMibTestPath("QuotedExeWithArgs");
					fs_TestParse("\"c:/Program Files/foo.exe\" --arg1", {"c:/Program Files/foo.exe", "--arg1"});
				}

				// The cmake_automoc_parser shape: quotes around an exe path that
				// happens to contain a backslash separator.
				{
					DMibTestPath("QuotedExeWithBackslash");
					fs_TestParse("\"c:/dev/libexec\\foo.exe\" --arg", {"c:/dev/libexec\\foo.exe", "--arg"});
				}

				// Args with embedded quotes use CRT-style \" escape.
				{
					DMibTestPath("ArgWithEscapedQuote");
					fs_TestParse("foo \"a\\\"b\"", {"foo", "a\"b"});
				}
			};

			DMibTestSuite("ParseEncodeRoundTrip")
			{
				// These are the inputs the ParseCommandLineWindows->EscapeWindows
				// DSL chain receives. Verify that round-tripping through a parse
				// + re-encode + parse cycle yields the same token list.
				{
					DMibTestPath("Bare");
					fs_TestParseEncodeRoundTrip("c:/Dev/foo.exe --arg");
				}
				{
					DMibTestPath("QuotedExe");
					fs_TestParseEncodeRoundTrip("\"c:/Program Files/foo.exe\" --arg");
				}
				{
					DMibTestPath("QuotedExeWithBackslash");
					fs_TestParseEncodeRoundTrip("\"c:/dev/libexec\\foo.exe\" --arg");
				}
				{
					DMibTestPath("MultipleQuotedArgs");
					fs_TestParseEncodeRoundTrip("\"c:/Program Files/foo.exe\" \"hello world\" plain");
				}
				{
					DMibTestPath("ShellOperators");
					fs_TestParseEncodeRoundTrip("\"c:/dev/foo.exe\" -o output && other.exe -p");
				}
			};

			DMibTestSuite("ExactOutput")
			{
				// Args without spaces or quotes pass through unquoted.
				{
					DMibTestPath("Unquoted");
					fs_TestEncoded({"foo", "bar", "baz"}, "foo bar baz");
				}

				// Backslashes in unquoted args are emitted literally (CRT only
				// treats backslashes specially before a quote).
				{
					DMibTestPath("UnquotedBackslashes");
					fs_TestEncoded({"c:\\Windows\\foo"}, "c:\\Windows\\foo");
				}

				// A space forces quoted form. Backslashes inside are still
				// literal because they are not before a quote.
				{
					DMibTestPath("QuotedSimple");
					fs_TestEncoded({"hello world"}, "\"hello world\"");
				}
				{
					DMibTestPath("QuotedWithBackslash");
					fs_TestEncoded({"c:\\Program Files\\foo"}, "\"c:\\Program Files\\foo\"");
				}

				// Embedded `"` triggers quoted form. Each embedded `"` is
				// escaped as 2N+1 backslashes + `"` where N is the number of
				// backslashes that immediately precede it.
				{
					DMibTestPath("EmbeddedQuoteNoBackslash");
					fs_TestEncoded({"a\"b"}, "\"a\\\"b\"");
				}
				{
					DMibTestPath("EmbeddedQuoteOneBackslash");
					fs_TestEncoded({"a\\\"b"}, "\"a\\\\\\\"b\"");
				}
				{
					DMibTestPath("EmbeddedQuoteTwoBackslashes");
					fs_TestEncoded({"a\\\\\"b"}, "\"a\\\\\\\\\\\"b\"");
				}

				// THE BUG: backslashes-not-before-quote inside a quoted arg used
				// to be silently dropped. Lock down the exact expected output.
				// Runtime arg = `"c:/dev/libexec\foo.exe"` (single backslash);
				// expected = `"\"c:/dev/libexec\foo.exe\""`.
				{
					DMibTestPath("BackslashesNotBeforeQuote single");
					fs_TestEncoded({"\"c:/dev/libexec\\foo.exe\""}, "\"\\\"c:/dev/libexec\\foo.exe\\\"\"");
				}
				// Runtime arg = `"c:/dev/libexec\\bar.exe"` (two backslashes);
				// expected = `"\"c:/dev/libexec\\bar.exe\""`.
				{
					DMibTestPath("BackslashesNotBeforeQuote double");
					fs_TestEncoded({"\"c:/dev/libexec\\\\bar.exe\""}, "\"\\\"c:/dev/libexec\\\\bar.exe\\\"\"");
				}

				// THE OTHER BUG: trailing backslashes before the closing `"`
				// must be doubled (2N) so they round-trip as N literal
				// backslashes, otherwise an odd N produces a literal `"` at
				// parse time instead of closing the quoted region.
				{
					DMibTestPath("TrailingBackslash 1");
					fs_TestEncoded({"foo bar\\"}, "\"foo bar\\\\\"");       // 1 in -> 2 emitted
				}
				{
					DMibTestPath("TrailingBackslash 2");
					fs_TestEncoded({"foo bar\\\\"}, "\"foo bar\\\\\\\\\""); // 2 in -> 4 emitted
				}
				{
					DMibTestPath("TrailingBackslash 3");
					fs_TestEncoded({"foo bar\\\\\\"}, "\"foo bar\\\\\\\\\\\\\""); // 3 in -> 6 emitted
				}

				// Empty args are emitted as "".
				{
					DMibTestPath("EmptyArg");
					fs_TestEncoded({"", "a"}, "\"\" a");
				}

				// Multiple args are space-separated.
				{
					DMibTestPath("Multiple");
					fs_TestEncoded({"a", "b c", "d"}, "a \"b c\" d");
				}
			};

			DMibTestSuite("RoundTrip")
			{
				{
					DMibTestPath("Simple");
					fs_TestRoundTrip({"foo", "bar", "baz"});
				}
				{
					DMibTestPath("WithSpaces");
					fs_TestRoundTrip({"hello world", "another arg"});
				}
				{
					DMibTestPath("WindowsPath");
					fs_TestRoundTrip({"c:\\Program Files\\App\\foo.exe", "/c", "build"});
				}

				// The exact case that triggered the cmake_automoc_parser failure:
				// `path\\foo.exe` with literal `\\` in the middle of an arg that
				// contains an embedded quote (forcing the quoted encoding path).
				{
					DMibTestPath("BackslashesNotBeforeQuote single");
					fs_TestRoundTrip({"\"c:/dev/libexec\\foo.exe\""});
				}
				{
					DMibTestPath("BackslashesNotBeforeQuote double");
					fs_TestRoundTrip({"\"c:/dev/libexec\\\\bar/baz.exe\""});
				}

				{
					DMibTestPath("EmbeddedQuote");
					fs_TestRoundTrip({"a\"b", "c\"\"d"});
				}
				{
					DMibTestPath("BackslashBeforeQuote");
					fs_TestRoundTrip({"a\\\"b", "a\\\\\"b"});
				}

				// Trailing backslashes in an arg that needs quoting (has a space)
				// must be doubled before the closing quote.
				{
					DMibTestPath("TrailingBackslashInQuotedArg 1");
					fs_TestRoundTrip({"foo bar\\"});
				}
				{
					DMibTestPath("TrailingBackslashInQuotedArg 2");
					fs_TestRoundTrip({"foo bar\\\\"});
				}
				{
					DMibTestPath("TrailingBackslashInQuotedArg 3");
					fs_TestRoundTrip({"foo bar\\\\\\"});
				}

				{
					DMibTestPath("EmptyArg");
					fs_TestRoundTrip({"", "a", "", "b"});
				}
				{
					DMibTestPath("OnlyBackslashes");
					fs_TestRoundTrip({"\\", "\\\\", "\\\\\\"});
				}
				{
					DMibTestPath("MixedSlashes");
					fs_TestRoundTrip({"a/b\\c d", "x\\y/z"});
				}
			};

#ifdef DPlatformFamily_Windows
			DMibTestSuite("CommandLineToArgvW")
			{
				{
					DMibTestPath("Simple");
					fs_TestRoundTripCommandLineToArgvW({"foo", "bar", "baz"});
				}
				{
					DMibTestPath("WithSpaces");
					fs_TestRoundTripCommandLineToArgvW({"hello world", "another arg"});
				}
				{
					DMibTestPath("WindowsPath");
					fs_TestRoundTripCommandLineToArgvW({"c:\\Program Files\\App\\foo.exe", "/c", "build"});
				}

				{
					DMibTestPath("BackslashesNotBeforeQuote single");
					fs_TestRoundTripCommandLineToArgvW({"\"c:/dev/libexec\\foo.exe\""});
				}
				{
					DMibTestPath("BackslashesNotBeforeQuote double");
					fs_TestRoundTripCommandLineToArgvW({"\"c:/dev/libexec\\\\bar/baz.exe\""});
				}

				{
					DMibTestPath("EmbeddedQuote");
					fs_TestRoundTripCommandLineToArgvW({"a\"b", "c\"\"d"});
				}
				{
					DMibTestPath("BackslashBeforeQuote");
					fs_TestRoundTripCommandLineToArgvW({"a\\\"b", "a\\\\\"b"});
				}

				{
					DMibTestPath("TrailingBackslashInQuotedArg 1");
					fs_TestRoundTripCommandLineToArgvW({"foo bar\\"});
				}
				{
					DMibTestPath("TrailingBackslashInQuotedArg 2");
					fs_TestRoundTripCommandLineToArgvW({"foo bar\\\\"});
				}
				{
					DMibTestPath("TrailingBackslashInQuotedArg 3");
					fs_TestRoundTripCommandLineToArgvW({"foo bar\\\\\\"});
				}

				{
					DMibTestPath("OnlyBackslashes");
					fs_TestRoundTripCommandLineToArgvW({"\\", "\\\\", "\\\\\\"});
				}
				{
					DMibTestPath("MixedSlashes");
					fs_TestRoundTripCommandLineToArgvW({"a/b\\c d", "x\\y/z"});
				}
			};
#endif
		}
	};

	DMibTestRegister(CProcessParamEncoding_Tests, Malterlib::Process);

}
