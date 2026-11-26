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
#include "oslib/http_client.h"
#include "stdclass.h"
#include <windows.h>
#include <memory>
#include <chrono>

namespace http {

using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage::Streams;
using namespace Windows::Web::Http;
using namespace Windows::Web::Http::Headers;

static HttpClient^ httpClient;

void init()
{
	nowide::wstackstring wagent;
	wagent.convert(getUserAgent().c_str());

	httpClient = ref new HttpClient();
	httpClient->DefaultRequestHeaders->UserAgent->ParseAdd(ref new String(wagent.get()));
}

void term() {
	httpClient = nullptr;
}

static void getHeaders(IIterator<IKeyValuePair<String^, String^>^>^ it, Headers& headers)
{
	while (it->HasCurrent)
	{
		String^ key = it->Current->Key;
		String^ value = it->Current->Value;
		nowide::stackstring nwkey;
		nowide::stackstring nwvalue;
		if (nwkey.convert(key->Data()) && nwvalue.convert(value->Data()))
		{
			std::string strkey(nwkey.get());
			string_tolower(strkey);
			headers.emplace_back(strkey, nwvalue.get());
		}
		it->MoveNext();
	}
}

int get(const std::string& url, std::vector<u8>& content, const Headers *reqHeaders, Headers *respHeaders, int timeoutMs, const std::atomic<bool> *cancelled, size_t maxBodyBytes)
{
	content.clear();
	if (cancelled && cancelled->load()) return 499;
	nowide::wstackstring wurl;
	if (!wurl.convert(url.c_str()))
		return 500;
	try
	{
		Uri^ uri = ref new Uri(ref new String(wurl.get()));
		HttpRequestMessage^ request = ref new HttpRequestMessage(HttpMethod::Get, uri);

		if (reqHeaders != nullptr)
		{
			for (const auto& [ name, value ] : *reqHeaders)
			{
				nowide::wstackstring wname;
				nowide::wstackstring wvalue;
				if (wname.convert(name.c_str()) && wvalue.convert(value.c_str()))
					request->Headers->Insert(ref new String(wname.get()), ref new String(wvalue.get()));
			}
		}

		IAsyncOperationWithProgress<HttpResponseMessage^, HttpProgress>^ op = httpClient->SendRequestAsync(request, HttpCompletionOption::ResponseHeadersRead);
		// Cancellation can complete after this function returns. Keep the event
		// alive until both WinRT completion callbacks have released their copies.
		auto asyncEvent = std::make_shared<cResetEvent>();
		const auto deadline = std::chrono::steady_clock::now()
			+ std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 30000);
		const auto wait = [&]() {
			while (!asyncEvent->Wait(25)) {
				if ((cancelled && cancelled->load()) || std::chrono::steady_clock::now() >= deadline) return false;
			}
			return !cancelled || !cancelled->load();
		};
		op->Completed = ref new AsyncOperationWithProgressCompletedHandler<HttpResponseMessage^, HttpProgress>(
				[asyncEvent](IAsyncOperationWithProgress<HttpResponseMessage^, HttpProgress>^, Windows::Foundation::AsyncStatus) {
					asyncEvent->Set();
		        });
		if (!wait())
		{
			op->Cancel();
			return cancelled && cancelled->load() ? 499 : 408;
		}
		HttpResponseMessage^ resp = op->GetResults();

		if (resp->IsSuccessStatusCode)
		{
			IHttpContent^ httpContent = resp->Content;

			if (respHeaders != nullptr) {
				getHeaders(resp->Headers->First(), *respHeaders);
				getHeaders(httpContent->Headers->First(), *respHeaders);
			}
			IReference<UINT64>^ declaredLength = httpContent->Headers->ContentLength;
			if (maxBodyBytes != unlimitedBody && declaredLength != nullptr && declaredLength->Value > maxBodyBytes) {
				resp->Close();
				return bodyTooLarge;
			}

			IAsyncOperationWithProgress<IInputStream^, uint64_t>^ streamOp = httpContent->ReadAsInputStreamAsync();
			asyncEvent->Reset();
			streamOp->Completed = ref new AsyncOperationWithProgressCompletedHandler<IInputStream^, uint64_t>(
				[asyncEvent](IAsyncOperationWithProgress<IInputStream^, uint64_t>^, Windows::Foundation::AsyncStatus) {
					asyncEvent->Set();
				});
			if (!wait())
			{
				streamOp->Cancel();
				resp->Close();
				return cancelled && cancelled->load() ? 499 : 408;
			}
			IInputStream^ input = streamOp->GetResults();
			for (;;) {
				Buffer^ requested = ref new Buffer(4096);
				IAsyncOperationWithProgress<IBuffer^, uint32_t>^ readOp = input->ReadAsync(requested, 4096, InputStreamOptions::None);
				asyncEvent->Reset();
				readOp->Completed = ref new AsyncOperationWithProgressCompletedHandler<IBuffer^, uint32_t>(
					[asyncEvent](IAsyncOperationWithProgress<IBuffer^, uint32_t>^, Windows::Foundation::AsyncStatus) {
						asyncEvent->Set();
					});
				if (!wait()) {
					readOp->Cancel();
					input->Close();
					resp->Close();
					content.clear();
					return cancelled && cancelled->load() ? 499 : 408;
				}
				IBuffer^ buffer = readOp->GetResults();
				if (buffer->Length == 0) break;
				Array<u8>^ array = ref new Array<u8>(buffer->Length);
				DataReader::FromBuffer(buffer)->ReadBytes(array);
				if (!appendBody(content, array->Data, buffer->Length, maxBodyBytes)) {
					input->Close();
					resp->Close();
					return bodyTooLarge;
				}
			}
			input->Close();
		}
		const int status = (int)resp->StatusCode;
		resp->Close();
		return status;
	}
	catch (Exception^ e)
	{
		content.clear();
		WARN_LOG(COMMON, "http::get error %.*S", e->Message->Length(), e->Message->Data());
		return cancelled && cancelled->load() ? 499 : 500;
	}
}

int post(const std::string& url, const char *payload, const char *contentType, std::vector<u8>& reply)
{
	nowide::wstackstring wurl;
	if (!wurl.convert(url.c_str()))
		return 500;
	nowide::wstackstring wpayload;
	if (!wpayload.convert(payload))
		return 500;
	nowide::wstackstring wcontentType;
	if (contentType != nullptr && !wcontentType.convert(contentType))
		return 500;
	try
	{
		Uri^ uri = ref new Uri(ref new String(wurl.get()));
		HttpStringContent^ content = ref new HttpStringContent(ref new String(wpayload.get()));
		content->Headers->ContentLength = ref new Box<UINT64>(strlen(payload));
		if (contentType != nullptr)
			content->Headers->ContentType = ref new HttpMediaTypeHeaderValue(ref new String(wcontentType.get()));

		IAsyncOperationWithProgress<HttpResponseMessage^, HttpProgress>^ op = httpClient->PostAsync(uri, content);
		cResetEvent asyncEvent;
		op->Completed = ref new AsyncOperationWithProgressCompletedHandler<HttpResponseMessage^, HttpProgress>(
			[&asyncEvent](IAsyncOperationWithProgress<HttpResponseMessage^, HttpProgress>^, Windows::Foundation::AsyncStatus) {
				asyncEvent.Set();
			});
		if (!asyncEvent.Wait(30000))
			return 408;
		HttpResponseMessage^ resp = op->GetResults();

		if (resp->IsSuccessStatusCode)
		{
			IHttpContent^ httpContent = resp->Content;
			IAsyncOperationWithProgress<IBuffer^, uint64_t>^ readOp = httpContent->ReadAsBufferAsync();
			asyncEvent.Reset();
			readOp->Completed = ref new AsyncOperationWithProgressCompletedHandler<IBuffer^, uint64_t>(
				[&asyncEvent](IAsyncOperationWithProgress<IBuffer^, uint64_t>^, Windows::Foundation::AsyncStatus) {
					asyncEvent.Set();
				});
			asyncEvent.Wait();
			IBuffer^ buffer = readOp->GetResults();

			Array<u8>^ array = ref new Array<u8>(buffer->Length);
			DataReader::FromBuffer(buffer)->ReadBytes(array);
			reply = std::vector<u8>(array->begin(), array->end());
		}
		return (int)resp->StatusCode;
	}
	catch (Exception^ e)
	{
		WARN_LOG(COMMON, "http::post error %.*S", e->Message->Length(), e->Message->Data());
		return 500;
	}
}

int post(const std::string & url, const std::vector<PostField>&fields) {
	// Not implemented on UWP.
	return 500;
}

}
