// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#import "Test_Malterlib_Process_Launchable_EventHandler.h"
#import "AppKit/NSApplication.h"
#include <Mib/Core/Core>

@implementation CMacOSEventHandler

- (void)getUrl: (NSAppleEventDescriptor *)_pEvent withReplyEvent: (NSAppleEventDescriptor *)_pReplyEvent
{
	// Get the URL
	if (m_pfOnOpenURL)
	{
		NSString *URLString = [[_pEvent paramDescriptorForKeyword: keyDirectObject] stringValue];

		NMib::NStr::CStr URL(URLString.UTF8String);

		m_pfOnOpenURL(URL);

		[[NSApplication sharedApplication] stop: NSApp];
	}
}

- (void)setOnOpenURL: (FOnOpenURL *)_pHandler
{
	m_pfOnOpenURL = _pHandler;
}
@end

CMacOSEventHandler *g_pMacOSEventHandler = nullptr;

void fg_CreateMacOSEventHandler(FOnOpenURL *_pOnOpenURL)
{
	g_pMacOSEventHandler = [[CMacOSEventHandler alloc] init];

	[g_pMacOSEventHandler setOnOpenURL: _pOnOpenURL];

	NSAppleEventManager *pEventManager = [NSAppleEventManager sharedAppleEventManager];
	[pEventManager setEventHandler: g_pMacOSEventHandler andSelector: @selector(getUrl:withReplyEvent:) forEventClass: kInternetEventClass andEventID: kAEGetURL];

	[pEventManager setEventHandler: g_pMacOSEventHandler andSelector: @selector(getUrl:withReplyEvent:) forEventClass: 'WWW!' andEventID: 'OURL'];
}

void fg_DestroyMacOSEventHandler()
{
	g_pMacOSEventHandler = nullptr;
}

void fg_RunApplicationMain(int _ArgC, char *_pArgV[])
{
	[NSApplication sharedApplication];

	[NSApp run];

	// NMib::NSys::fg_Thread_Sleep(1.0);

	NMib::NSys::fg_TerminateProcess(0);

	// NSApplicationMain(0, nullptr);
}
