// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#import <Foundation/Foundation.h>

#import <Mib/Core/Core>

typedef void (FOnOpenURL)(NMib::NStr::CStr const& _URL);

@interface CMacOSEventHandler : NSObject
{

	FOnOpenURL* m_pfOnOpenURL;
}
@end
