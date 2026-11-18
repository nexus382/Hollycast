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
#include <condition_variable>
#include <memory>
#include <mutex>

namespace http {

int get(const std::string& url, std::vector<u8>& content, const Headers *reqHeaders, Headers *respHeaders, int timeoutMs, const std::atomic<bool> *cancelled)
{
	if (cancelled && cancelled->load()) return 499;
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
	if (cancelled != nullptr) {
		struct Result {
			std::mutex mutex;
			std::condition_variable changed;
			bool done = false;
			int status = 500;
			std::vector<u8> bytes;
			Headers headers;
		};
		auto result = std::make_shared<Result>();
		NSURLSessionDataTask *task = [[NSURLSession sharedSession] dataTaskWithRequest:urlRequest
			completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
				std::lock_guard<std::mutex> lock(result->mutex);
				if (error == nil && [response isKindOfClass:[NSHTTPURLResponse class]]) {
					NSHTTPURLResponse *reply = (NSHTTPURLResponse *)response;
					result->status = (int)reply.statusCode;
					if (data.length != 0) result->bytes.assign((const u8 *)data.bytes, (const u8 *)data.bytes + data.length);
					for (NSString *key in reply.allHeaderFields) {
						std::string name([key UTF8String]);
						string_tolower(name);
						result->headers.emplace_back(name, [[reply.allHeaderFields[key] description] UTF8String]);
					}
				}
				result->done = true;
				result->changed.notify_one();
			}];
		[task resume];
		std::unique_lock<std::mutex> lock(result->mutex);
		while (!result->done) {
			result->changed.wait_for(lock, std::chrono::milliseconds(25));
			if (cancelled->load()) [task cancel];
		}
		content = std::move(result->bytes);
		if (respHeaders) *respHeaders = std::move(result->headers);
		return cancelled->load() ? 499 : result->status;
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
