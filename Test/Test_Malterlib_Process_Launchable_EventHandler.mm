// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#import "Test_Malterlib_Process_Launchable_EventHandler.h"
#import "AppKit/NSApplication.h"
#include <Mib/Core/Core>

@implementation CMacOSEventHandler

- (void)getUrl:(NSAppleEventDescriptor *)event 
	withReplyEvent:(NSAppleEventDescriptor *)replyEvent
{
	// Get the URL
	if (m_pfOnOpenURL)
	{
		NSString *URLString = [[event paramDescriptorForKeyword:keyDirectObject] stringValue];

		NSData *pData = [URLString dataUsingEncoding: NSUTF8StringEncoding];
		NMib::NStr::CStr URL{(ch8 const *)pData.bytes, pData.length};

		m_pfOnOpenURL(URL);

		[[NSApplication sharedApplication] stop: NSApp];
	}	
}

-(void)setOnOpenURL:(FOnOpenURL *)_pfHandler
{
	m_pfOnOpenURL = _pfHandler;
}
@end

CMacOSEventHandler* g_pMacOSEventHandler = nullptr;

void fg_CreateMacOSEventHandler(FOnOpenURL *_pfOnOpenURL)
{
	g_pMacOSEventHandler = [[CMacOSEventHandler alloc] init];

	[g_pMacOSEventHandler setOnOpenURL: _pfOnOpenURL];

	NSAppleEventManager *pEventManager = [NSAppleEventManager sharedAppleEventManager];
	[pEventManager 
		setEventHandler:g_pMacOSEventHandler
		andSelector:@selector(getUrl:withReplyEvent:) 
		forEventClass:kInternetEventClass 
		andEventID:kAEGetURL
	];	

	[pEventManager
		setEventHandler:g_pMacOSEventHandler
		andSelector:@selector(getUrl:withReplyEvent:) 
		forEventClass:'WWW!' 
		andEventID:'OURL'
	];
}

void fg_DestroyMacOSEventHandler()
{
	g_pMacOSEventHandler = nullptr;
}

void fg_RunApplicationMain(int argc, char *argv[])
{

	[NSApplication sharedApplication];

	[NSApp run];

//	NMib::NSys::fg_Thread_Sleep(1.0);

	NMib::NSys::fg_TerminateProcess(0);

//	NSApplicationMain(0, nullptr);

}
