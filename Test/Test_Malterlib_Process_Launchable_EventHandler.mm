// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#import "Test_Malterlib_Process_Launchable_EventHandler.h"
#import "AppKit/NSApplication.h"
#include <Mib/Core/Core>

@implementation COSXEventHandler

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

COSXEventHandler* g_pOSXEventHandler = nullptr;
NSAutoreleasePool *pAutoReleasePool = nullptr;


void fg_CreateOSXEventHandler(FOnOpenURL *_pfOnOpenURL)
{
	pAutoReleasePool = [[NSAutoreleasePool alloc] init];

	g_pOSXEventHandler = [[COSXEventHandler alloc] init];

	[g_pOSXEventHandler setOnOpenURL: _pfOnOpenURL];

	NSAppleEventManager *pEventManager = [NSAppleEventManager sharedAppleEventManager];
	[pEventManager 
		setEventHandler:g_pOSXEventHandler 
		andSelector:@selector(getUrl:withReplyEvent:) 
		forEventClass:kInternetEventClass 
		andEventID:kAEGetURL
	];	

	[pEventManager
		setEventHandler:g_pOSXEventHandler 
		andSelector:@selector(getUrl:withReplyEvent:) 
		forEventClass:'WWW!' 
		andEventID:'OURL'
	];
}

void fg_DestroyOSXEventHandler()
{
	[g_pOSXEventHandler release];
	g_pOSXEventHandler = nullptr;

	[pAutoReleasePool release];
}

void fg_RunApplicationMain(int argc, char *argv[])
{

	[NSApplication sharedApplication];

	[NSApp run];

//	NMib::NSys::fg_Thread_Sleep(1.0);

	NMib::NSys::fg_TerminateProcess(0);

	[NSApp release];

//	NSApplicationMain(0, nullptr);

}
