/*
	Copyright 2022 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	Flycast is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#import <Foundation/Foundation.h>
#include "oslib/http_client.h"
#include "stdclass.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <utility>

// Keep callback output separate from the caller's vector so cancellation cannot
// leave an asynchronous delegate writing through a returned stack reference.
struct BoundedGetState
{
	http::Headers headers;
	std::vector<u8> content;
	NSInteger statusCode = 500;
	int failure = 0;
	bool done = false;
};

@interface HollycastBoundedGetDelegate : NSObject <NSURLConnectionDataDelegate>
{
	std::shared_ptr<BoundedGetState> _state;
	size_t _maxBytes;
}
- (instancetype)initWithState:(std::shared_ptr<BoundedGetState>)state maxBytes:(size_t)maxBytes;
@end

@implementation HollycastBoundedGetDelegate
- (instancetype)initWithState:(std::shared_ptr<BoundedGetState>)state maxBytes:(size_t)maxBytes
{
	self = [super init];
	if (self != nil)
	{
		_state = std::move(state);
		_maxBytes = maxBytes;
	}
	return self;
}

- (void)connection:(NSURLConnection *)connection didReceiveResponse:(NSURLResponse *)response
{
	// Redirects can deliver another response; only retain the final response body.
	_state->content.clear();
	_state->headers.clear();
	NSHTTPURLResponse *httpResponse = (NSHTTPURLResponse *)response;
	_state->statusCode = [httpResponse statusCode];
	for (id key in [httpResponse allHeaderFields])
	{
		std::string name([key UTF8String]);
		string_tolower(name);
		std::string value([[httpResponse.allHeaderFields objectForKey:key] UTF8String]);
		_state->headers.emplace_back(std::move(name), std::move(value));
	}
}

- (void)connection:(NSURLConnection *)connection didReceiveData:(NSData *)data
{
	if (_state->failure != 0)
		return;
	const size_t size = [data length];
	if (size > _maxBytes - _state->content.size())
	{
		_state->content.clear();
		_state->failure = 413;
		_state->done = true;
		[connection cancel];
		return;
	}
	if (size == 0)
		return;
	const u8 *begin = (const u8 *)[data bytes];
	_state->content.insert(_state->content.end(), begin, begin + size);
}

- (void)connectionDidFinishLoading:(NSURLConnection *)connection
{
	_state->done = true;
}

- (void)connection:(NSURLConnection *)connection didFailWithError:(NSError *)error
{
	if (_state->failure == 0)
		_state->failure = 500;
	_state->done = true;
}
@end

namespace http {

int get(const std::string& url, std::vector<u8>& content, const Headers *reqHeaders, Headers *respHeaders,
		int timeoutMs, size_t maxBytes, const std::atomic_bool *cancelled)
{
	NSString *nsurl = [NSString stringWithCString:url.c_str() 
                                         encoding:[NSString defaultCStringEncoding]];
    NSString *userAgent = [NSString stringWithCString:getUserAgent().c_str()
                                             encoding:[NSString defaultCStringEncoding]];
	NSMutableURLRequest *urlRequest = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:nsurl]];
	[urlRequest setValue:userAgent forHTTPHeaderField:@"User-Agent"];
    if (reqHeaders != nullptr)
    {
        for (const auto& [ name, value ] : *reqHeaders)
        {
            NSString *nsname = [NSString stringWithCString:name.c_str()
                                                  encoding:[NSString defaultCStringEncoding]];
            NSString *nsvalue = [NSString stringWithCString:value.c_str()
                                                   encoding:[NSString defaultCStringEncoding]];
            [urlRequest setValue:nsvalue forHTTPHeaderField:nsname];
        }
    }
    if (timeoutMs > 0)
        [urlRequest setTimeoutInterval:timeoutMs / 1000.0];
	if (maxBytes != std::numeric_limits<size_t>::max() || cancelled != nullptr)
	{
		@autoreleasepool
		{
			content.clear();
			if (cancelled != nullptr && cancelled->load(std::memory_order_relaxed))
				return 499;

			const int timeout = timeoutMs > 0 ? timeoutMs : 30000;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
			auto state = std::make_shared<BoundedGetState>();
			HollycastBoundedGetDelegate *delegate = [[HollycastBoundedGetDelegate alloc] initWithState:state maxBytes:maxBytes];
			NSURLConnection *connection = [[NSURLConnection alloc] initWithRequest:urlRequest delegate:delegate startImmediately:NO];
#if !__has_feature(objc_arc)
			// macOS and libretro also build this file without ARC.
			[delegate autorelease];
			[connection autorelease];
#endif
			if (connection == nil)
				return 500;
			NSRunLoop *runLoop = [NSRunLoop currentRunLoop];
			[connection scheduleInRunLoop:runLoop forMode:NSDefaultRunLoopMode];
			[connection start];
			// NSURLConnection delivers chunks on this run loop; short waits enforce the
			// overall deadline while letting the caller's cancellation flag be polled.
			while (!state->done)
			{
				if (cancelled != nullptr && cancelled->load(std::memory_order_relaxed))
				{
					state->failure = 499;
					break;
				}
				const auto now = std::chrono::steady_clock::now();
				if (now >= deadline)
				{
					state->failure = 408;
					break;
				}
				const auto wait = std::min(std::chrono::duration<double>(deadline - now).count(), 0.05);
				[runLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:wait]];
			}
			if (cancelled != nullptr && cancelled->load(std::memory_order_relaxed))
				state->failure = 499;
			else if (state->failure == 0 && std::chrono::steady_clock::now() >= deadline)
				state->failure = 408;
			[connection cancel];
			[connection unscheduleFromRunLoop:runLoop forMode:NSDefaultRunLoopMode];

			if (state->failure != 0)
			{
				state->content.clear();
				return state->failure;
			}
			if (respHeaders != nullptr)
				respHeaders->insert(respHeaders->end(), state->headers.begin(), state->headers.end());
			content = std::move(state->content);
			return (int)state->statusCode;
		}
	}

	NSURLResponse *response = nil;
	NSError *error = nil;
	NSData *data = [NSURLConnection sendSynchronousRequest:urlRequest
                                         returningResponse:&response
                                                     error:&error];
	if (error != nil)
		return 500;

	NSHTTPURLResponse *httpResponse = (NSHTTPURLResponse *)response;
	if (respHeaders != nullptr)
	{
		NSDictionary *headers = httpResponse.allHeaderFields;
		NSEnumerator *enumerator = [headers keyEnumerator];
		id key;
		while ((key = [enumerator nextObject]))
		{
			std::string strkey([key UTF8String]);
			string_tolower(strkey);
			std::string strvalue([[headers objectForKey:key] UTF8String]);
			respHeaders->emplace_back(strkey, strvalue);
		}
	}
		
	content.clear();
	content.insert(content.begin(), (const u8 *)[data bytes], (const u8 *)[data bytes] + [data length]);
	
	return [httpResponse statusCode];
}

int post(const std::string& url, const char *payload, const char *contentType, std::vector<u8>& reply)
{
	NSString *nsurl = [NSString stringWithCString:url.c_str() 
                                         encoding:[NSString defaultCStringEncoding]];
    NSString *userAgent = [NSString stringWithCString:getUserAgent().c_str()
                                             encoding:[NSString defaultCStringEncoding]];
	NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:nsurl]];
	[request setHTTPMethod:@"POST"];
	[request setCachePolicy:NSURLRequestReloadIgnoringLocalCacheData];
	[request setHTTPShouldHandleCookies:NO];
	
	size_t payloadSize = strlen(payload);
	[request setHTTPBody:[NSData dataWithBytes:payload length:payloadSize]];
	NSString *postLength = [NSString stringWithFormat:@"%ld", (unsigned long)payloadSize];
    [request setValue:postLength forHTTPHeaderField:@"Content-Length"];
    NSString *nscontentType = contentType != nullptr ? [NSString stringWithCString:contentType 
                                         encoding:[NSString defaultCStringEncoding]]
    	: @"application/x-www-form-urlencoded";
    [request setValue:nscontentType forHTTPHeaderField:@"Content-Type"];
	[request setValue:userAgent forHTTPHeaderField:@"User-Agent"];
    
	NSURLResponse *response = nil;
	NSError *error = nil;
	NSData *data = [NSURLConnection sendSynchronousRequest:request
                          returningResponse:&response
                                      error:&error];
	if (error != nil)
		return 500;

	NSHTTPURLResponse *httpResponse = (NSHTTPURLResponse *)response; 
		
	reply.clear();
	reply.insert(reply.begin(), (const u8 *)[data bytes], (const u8 *)[data bytes] + [data length]);

	return [httpResponse statusCode];
}

int post(const std::string& url, const std::vector<PostField>& fields)
{
	NSString *nsurl = [NSString stringWithCString:url.c_str() 
                                         encoding:[NSString defaultCStringEncoding]];
    NSString *userAgent = [NSString stringWithCString:getUserAgent().c_str()
                                             encoding:[NSString defaultCStringEncoding]];
	NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:nsurl]];
	[request setHTTPMethod:@"POST"];
	[request setCachePolicy:NSURLRequestReloadIgnoringLocalCacheData];
	[request setHTTPShouldHandleCookies:NO];

	NSString *boundary = @"----flycast-boundary-7192397596";
	NSString *contentType = [NSString stringWithFormat:@"multipart/form-data; boundary=%@", boundary];
	[request setValue:contentType forHTTPHeaderField: @"Content-Type"];
	[request setValue:userAgent forHTTPHeaderField:@"User-Agent"];
	
	NSMutableData *body = [NSMutableData data];
	for (const PostField& field : fields)
	{
        NSString *value = [NSString stringWithCString:field.value.c_str() 
                                         encoding:[NSString defaultCStringEncoding]];
		[body appendData:[[NSString stringWithFormat:@"--%@\r\n", boundary] dataUsingEncoding:NSUTF8StringEncoding]];
		[body appendData:[[NSString stringWithFormat:@"Content-Disposition: form-data; name=\"%@\"", [NSString stringWithCString:field.name.c_str() 
                                         encoding:[NSString defaultCStringEncoding]]] dataUsingEncoding:NSUTF8StringEncoding]];
        if (!field.contentType.empty())
        {
	        [body appendData:[[NSString stringWithFormat:@"; filename=\"%@\"\r\n", value] dataUsingEncoding:NSUTF8StringEncoding]];
	        [body appendData:[[NSString stringWithFormat:@"Content-Type: %@", [NSString stringWithCString:field.contentType.c_str() 
                                         encoding:[NSString defaultCStringEncoding]]] dataUsingEncoding:NSUTF8StringEncoding]];
        }
        [body appendData:[@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding]];
        
        if (field.contentType.empty())
        {
	        [body appendData:[value dataUsingEncoding:NSUTF8StringEncoding]];
        }
        else
        {
        	NSError* error = nil;
			NSData *filedata = [NSData dataWithContentsOfFile:value options:0 error:&error];
			if (error != nil)
				return 500;
	        [body appendData:filedata];
        }
        [body appendData:[@"\r\n" dataUsingEncoding:NSUTF8StringEncoding]];
	}
	[body appendData:[[NSString stringWithFormat:@"--%@--\r\n", boundary] dataUsingEncoding:NSUTF8StringEncoding]];
	
	[request setHTTPBody:body];
	NSString *postLength = [NSString stringWithFormat:@"%ld", (unsigned long)[body length]];
    [request setValue:postLength forHTTPHeaderField:@"Content-Length"];
    
	NSURLResponse *response = nil;
	NSError *error = nil;
	[NSURLConnection sendSynchronousRequest:request
                          returningResponse:&response
                                      error:&error];
	if (error != nil)
		return 500;

	NSHTTPURLResponse *httpResponse = (NSHTTPURLResponse *)response; 
	return [httpResponse statusCode];
}

void init() {
}

void term() {
}

}
