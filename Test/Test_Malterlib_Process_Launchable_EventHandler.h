// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#import <Foundation/Foundation.h>

#import <Mib/Core/Core>

using FOnOpenURL = void (NMib::NStr::CStr const &_URL);

@interface CMacOSEventHandler : NSObject
{
	FOnOpenURL* m_pfOnOpenURL;
}
@end
