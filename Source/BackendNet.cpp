#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "Backend.h"

namespace {
	constexpr std::size_t maxRequest = 4 * 1024 * 1024;

	PCCERT_CONTEXT GenerateCertificate() {
		HCRYPTPROV randomProvider = 0;
		if (!CryptAcquireContextW(&randomProvider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
			throw std::runtime_error("Could not initialize certificate generator");
		BYTE random[16]{};
		const BOOL generated = CryptGenRandom(randomProvider, sizeof(random), random);
		CryptReleaseContext(randomProvider, 0);
		if (!generated)
			throw std::runtime_error("Could not generate certificate identifier");
		static constexpr wchar_t digits[] = L"0123456789abcdef";
		std::wstring container = L"LegacyEvolve-";
		for (const auto byte : random) {
			container += digits[byte >> 4];
			container += digits[byte & 15];
		}
		HCRYPTPROV provider = 0;
		if (!CryptAcquireContextW(&provider, container.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_NEWKEYSET))
			throw std::runtime_error("Could not create TLS key container");
		HCRYPTKEY key = 0;
		PCCERT_CONTEXT certificate = nullptr;
		do {
			if (!CryptGenKey(provider, AT_KEYEXCHANGE, (2048u << 16) | CRYPT_EXPORTABLE, &key))
				break;
			const wchar_t* hosts[] = {L"localhost", L"*.my.2k.com", L"my.2k.com", L"*.evolve2k.com", L"evolve2k.com",
			    L"*.4v1game.net", L"evolve.hydra.agoragames.com"};
			CERT_ALT_NAME_ENTRY names[8]{};
			for (unsigned i = 0; i < 7; ++i) {
				names[i].dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
				names[i].pwszDNSName = const_cast<wchar_t*>(hosts[i]);
			}
			BYTE loopback[] = {127, 0, 0, 1};
			names[7].dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
			names[7].IPAddress = {sizeof(loopback), loopback};
			CERT_ALT_NAME_INFO alternatives{8, names};
			CERT_BASIC_CONSTRAINTS2_INFO constraints{TRUE, FALSE, 0};
			BYTE usage =
			    CERT_DIGITAL_SIGNATURE_KEY_USAGE | CERT_KEY_ENCIPHERMENT_KEY_USAGE | CERT_KEY_CERT_SIGN_KEY_USAGE;
			CRYPT_BIT_BLOB keyUsage{1, &usage, 0};
			LPSTR serverAuth = const_cast<char*>(szOID_PKIX_KP_SERVER_AUTH);
			CERT_ENHKEY_USAGE enhancedUsage{1, &serverAuth};
			const void* values[] = {&alternatives, &constraints, &keyUsage, &enhancedUsage};
			const char* types[] = {
			    X509_ALTERNATE_NAME, X509_BASIC_CONSTRAINTS2, X509_KEY_USAGE, X509_ENHANCED_KEY_USAGE};
			const char* oids[] = {
			    szOID_SUBJECT_ALT_NAME2, szOID_BASIC_CONSTRAINTS2, szOID_KEY_USAGE, szOID_ENHANCED_KEY_USAGE};
			std::vector<BYTE> encoded[4];
			CERT_EXTENSION extensions[4]{};
			bool extensionsReady = true;
			for (unsigned i = 0; i < 4; ++i) {
				DWORD size = 0;
				if (!CryptEncodeObjectEx(X509_ASN_ENCODING, types[i], values[i], 0, nullptr, nullptr, &size)) {
					extensionsReady = false;
					break;
				}
				encoded[i].resize(size);
				if (!CryptEncodeObjectEx(
				        X509_ASN_ENCODING, types[i], values[i], 0, nullptr, encoded[i].data(), &size)) {
					extensionsReady = false;
					break;
				}
				extensions[i] = {const_cast<char*>(oids[i]), i == 1 || i == 2, {size, encoded[i].data()}};
			}
			if (!extensionsReady)
				break;
			CERT_EXTENSIONS certificateExtensions{4, extensions};
			DWORD nameSize = 0;
			const wchar_t* subject = L"CN=Evolve LAN local backend";
			if (!CertStrToNameW(X509_ASN_ENCODING, subject, CERT_X500_NAME_STR, nullptr, nullptr, &nameSize, nullptr))
				break;
			std::vector<BYTE> name(nameSize);
			if (!CertStrToNameW(
			        X509_ASN_ENCODING, subject, CERT_X500_NAME_STR, nullptr, name.data(), &nameSize, nullptr))
				break;
			CERT_NAME_BLOB issuer{nameSize, name.data()};
			CRYPT_KEY_PROV_INFO info{};
			info.pwszContainerName = const_cast<wchar_t*>(container.c_str());
			info.pwszProvName = const_cast<wchar_t*>(MS_ENH_RSA_AES_PROV_W);
			info.dwProvType = PROV_RSA_AES;
			info.dwKeySpec = AT_KEYEXCHANGE;
			CRYPT_ALGORITHM_IDENTIFIER algorithm{};
			algorithm.pszObjId = const_cast<char*>(szOID_RSA_SHA256RSA);
			SYSTEMTIME start{};
			GetSystemTime(&start);
			SYSTEMTIME end = start;
			end.wYear = static_cast<WORD>(end.wYear + 20);
			if (end.wMonth == 2 && end.wDay == 29)
				end.wDay = 28;
			certificate = CertCreateSelfSignCertificate(
			    provider, &issuer, 0, &info, &algorithm, &start, &end, &certificateExtensions);
		} while (false);
		if (key)
			CryptDestroyKey(key);
		CryptReleaseContext(provider, 0);
		if (!certificate) {
			HCRYPTPROV cleanup = 0;
			CryptAcquireContextW(&cleanup, container.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_DELETEKEYSET);
			throw std::runtime_error("Could not generate local TLS certificate");
		}
		return certificate;
	}

	void RemoveKey(PCCERT_CONTEXT certificate) {
		DWORD size = 0;
		if (!CertGetCertificateContextProperty(certificate, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size))
			return;
		std::vector<BYTE> data(size);
		if (!CertGetCertificateContextProperty(certificate, CERT_KEY_PROV_INFO_PROP_ID, data.data(), &size))
			return;
		const auto key = reinterpret_cast<CRYPT_KEY_PROV_INFO*>(data.data());
		HCRYPTPROV provider = 0;
		CryptAcquireContextW(&provider, key->pwszContainerName, key->pwszProvName, key->dwProvType, CRYPT_DELETEKEYSET);
	}

	PCCERT_CONTEXT RuntimeCertificate() {
		static const auto certificate = GenerateCertificate();
		return certificate;
	}

	struct Server {
		SOCKET listener = INVALID_SOCKET;
		CredHandle credentials{};
		bool acquired = false;
		unsigned short port = 0;
		std::atomic<unsigned> clients{0};

		~Server() {
			if (listener != INVALID_SOCKET)
				closesocket(listener);
			if (acquired)
				FreeCredentialsHandle(&credentials);
		}

		void Initialize() {
			auto certificate = RuntimeCertificate();
			SCHANNEL_CRED config{};
			config.dwVersion = SCHANNEL_CRED_VERSION;
			config.cCreds = 1;
			config.paCred = &certificate;
			config.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER;
			config.dwFlags = SCH_CRED_NO_SYSTEM_MAPPER;
			TimeStamp expiry{};
			const auto status = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W),
			    SECPKG_CRED_INBOUND, nullptr, &config, nullptr, nullptr, &credentials, &expiry);
			RemoveKey(certificate);
			if (status != SEC_E_OK)
				throw std::runtime_error("Windows could not initialize local TLS");
			acquired = true;
			listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
			    listen(listener, 16))
				throw std::runtime_error("Could not listen on loopback");
			int length = sizeof(address);
			if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length))
				throw std::runtime_error("Could not read loopback port");
			port = ntohs(address.sin_port);
		}
	};

	struct Connection {
		Server* server;
		SOCKET socket;
		CtxtHandle context{};
		bool initialized = false;
		SecPkgContext_StreamSizes sizes{};
		std::vector<char> incoming;

		explicit Connection(Server* owner, SOCKET value) : server(owner), socket(value) {
			DWORD timeout = 10000;
			setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
			setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
		}

		~Connection() {
			if (initialized)
				DeleteSecurityContext(&context);
			closesocket(socket);
		}

		bool Send(const char* data, std::size_t size) {
			while (size) {
				const auto sent = send(socket, data, static_cast<int>(size), 0);
				if (sent <= 0)
					return false;
				data += sent;
				size -= static_cast<std::size_t>(sent);
			}
			return true;
		}

		bool Receive() {
			char data[16384];
			const auto count = recv(socket, data, sizeof(data), 0);
			if (count <= 0 || incoming.size() + static_cast<std::size_t>(count) > maxRequest)
				return false;
			incoming.insert(incoming.end(), data, data + count);
			return true;
		}

		void KeepExtra(ULONG extra) {
			if (extra > incoming.size()) {
				incoming.clear();
				return;
			}
			incoming.erase(incoming.begin(), incoming.end() - extra);
		}

		bool Handshake() {
			if (!Receive())
				return false;
			for (;;) {
				SecBuffer in[2]{{static_cast<ULONG>(incoming.size()), SECBUFFER_TOKEN, incoming.data()},
				    {0, SECBUFFER_EMPTY, nullptr}};
				SecBufferDesc input{SECBUFFER_VERSION, 2, in};
				SecBuffer token{0, SECBUFFER_TOKEN, nullptr};
				SecBufferDesc output{SECBUFFER_VERSION, 1, &token};
				ULONG attributes = 0;
				TimeStamp expiry{};
				const auto status =
				    AcceptSecurityContext(&server->credentials, initialized ? &context : nullptr, &input,
				        ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_CONFIDENTIALITY | ASC_REQ_REPLAY_DETECT |
				            ASC_REQ_SEQUENCE_DETECT,
				        SECURITY_NATIVE_DREP, &context, &output, &attributes, &expiry);
				if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED)
					initialized = true;
				bool sent = true;
				if (token.pvBuffer) {
					sent = Send(static_cast<const char*>(token.pvBuffer), token.cbBuffer);
					FreeContextBuffer(token.pvBuffer);
				}
				if (!sent)
					return false;
				if (status == SEC_E_INCOMPLETE_MESSAGE) {
					if (!Receive())
						return false;
					continue;
				}
				if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED)
					return false;
				KeepExtra(in[1].BufferType == SECBUFFER_EXTRA ? in[1].cbBuffer : 0);
				if (status == SEC_E_OK)
					return QueryContextAttributesW(&context, SECPKG_ATTR_STREAM_SIZES, &sizes) == SEC_E_OK;
				if (incoming.empty() && !Receive())
					return false;
			}
		}

		bool Read(std::string& plain) {
			for (;;) {
				if (incoming.empty() && !Receive())
					return false;
				SecBuffer buffers[4]{{static_cast<ULONG>(incoming.size()), SECBUFFER_DATA, incoming.data()},
				    {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
				SecBufferDesc message{SECBUFFER_VERSION, 4, buffers};
				const auto status = DecryptMessage(&context, &message, 0, nullptr);
				if (status == SEC_E_INCOMPLETE_MESSAGE) {
					if (!Receive())
						return false;
					continue;
				}
				if (status != SEC_E_OK)
					return false;
				ULONG extra = 0;
				std::size_t added = 0;
				for (const auto& buffer : buffers) {
					if (buffer.BufferType == SECBUFFER_DATA) {
						if (plain.size() + buffer.cbBuffer > maxRequest)
							return false;
						plain.append(static_cast<const char*>(buffer.pvBuffer), buffer.cbBuffer);
						added += buffer.cbBuffer;
					}
					if (buffer.BufferType == SECBUFFER_EXTRA)
						extra = buffer.cbBuffer;
				}
				KeepExtra(extra);
				if (added)
					return true;
			}
		}

		bool Write(std::string_view plain) {
			while (!plain.empty()) {
				const auto count = static_cast<ULONG>(std::min<std::size_t>(plain.size(), sizes.cbMaximumMessage));
				std::vector<char> data(sizes.cbHeader + count + sizes.cbTrailer);
				memcpy(data.data() + sizes.cbHeader, plain.data(), count);
				SecBuffer buffers[4]{{sizes.cbHeader, SECBUFFER_STREAM_HEADER, data.data()},
				    {count, SECBUFFER_DATA, data.data() + sizes.cbHeader},
				    {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, data.data() + sizes.cbHeader + count},
				    {0, SECBUFFER_EMPTY, nullptr}};
				SecBufferDesc message{SECBUFFER_VERSION, 4, buffers};
				if (EncryptMessage(&context, 0, &message, 0) != SEC_E_OK)
					return false;
				for (const auto& buffer : buffers)
					if (buffer.cbBuffer && !Send(static_cast<const char*>(buffer.pvBuffer), buffer.cbBuffer))
						return false;
				plain.remove_prefix(count);
			}
			return true;
		}

		void Serve() {
			if (!Handshake())
				return;
			std::string request;
			std::size_t headerEnd;
			while ((headerEnd = request.find("\r\n\r\n")) == std::string::npos) {
				if (request.size() > 32768 || !Read(request))
					return;
			}
			const auto lineEnd = request.find("\r\n");
			const auto firstSpace = request.find(' ');
			const auto secondSpace = request.find(' ', firstSpace + 1);
			if (firstSpace == std::string::npos || secondSpace == std::string::npos || secondSpace > lineEnd)
				return;
			const auto path = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
			auto headers = request.substr(lineEnd, headerEnd - lineEnd);
			std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) {
				return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
			});
			std::size_t length = 0;
			const auto field = headers.find("\r\ncontent-length:");
			if (field != std::string::npos) {
				const auto start = field + strlen("\r\ncontent-length:");
				char* end = nullptr;
				const auto value = strtoull(headers.c_str() + start, &end, 10);
				if (end == headers.c_str() + start || value > maxRequest)
					return;
				length = static_cast<std::size_t>(value);
			}
			if (headers.find("\r\ntransfer-encoding:") != std::string::npos)
				return;
			if (headers.find("100-continue") != std::string::npos && !Write("HTTP/1.1 100 Continue\r\n\r\n"))
				return;
			const auto bodyStart = headerEnd + 4;
			while (request.size() - bodyStart < length)
				if (!Read(request))
					return;
			const auto response = offline::Respond(
			    std::wstring(path.begin(), path.end()), std::string_view(request).substr(bodyStart, length));
			const auto reply = "HTTP/1.1 " + std::to_string(response.status) +
			                   (response.status == 200 ? " OK\r\n" : " Not Found\r\n") +
			                   "Content-Type: " + response.type +
			                   "\r\nContent-Length: " + std::to_string(response.body.size()) +
			                   "\r\nConnection: close\r\n\r\n" + response.body;
			Write(reply);
		}
	};

	struct Work {
		Server* server;
		SOCKET socket;
	};

	DWORD WINAPI Client(void* data) {
		std::unique_ptr<Work> work(static_cast<Work*>(data));
		try {
			Connection(work->server, work->socket).Serve();
		} catch (...) {
		}
		--work->server->clients;
		return 0;
	}

	DWORD WINAPI Accept(void* data) {
		const auto server = static_cast<Server*>(data);
		for (;;) {
			const auto client = accept(server->listener, nullptr, nullptr);
			if (client == INVALID_SOCKET)
				return 0;
			if (server->clients.fetch_add(1) >= 16) {
				--server->clients;
				closesocket(client);
				continue;
			}
			auto work = std::make_unique<Work>(Work{server, client});
			if (!QueueUserWorkItem(Client, work.get(), WT_EXECUTELONGFUNCTION)) {
				--server->clients;
				closesocket(client);
			} else
				work.release();
		}
	}

	Server& LocalServer() {
		static auto server = [] {
			auto value = std::make_unique<Server>();
			value->Initialize();
			HMODULE pinned = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			        reinterpret_cast<LPCWSTR>(&Accept), &pinned))
				throw std::runtime_error("Could not retain local backend");
			const auto thread = CreateThread(nullptr, 0, Accept, value.get(), 0, nullptr);
			if (!thread)
				throw std::runtime_error("Could not start local backend");
			CloseHandle(thread);
			return value.release();
		}();
		return *server;
	}
}

namespace offline {
	std::string RuntimeCertificatePem() {
		const auto certificate = RuntimeCertificate();
		DWORD length = 0;
		if (!CryptBinaryToStringA(
		        certificate->pbCertEncoded, certificate->cbCertEncoded, CRYPT_STRING_BASE64HEADER, nullptr, &length))
			throw std::runtime_error("Could not encode local TLS certificate");
		std::string pem(length, '\0');
		if (!CryptBinaryToStringA(
		        certificate->pbCertEncoded, certificate->cbCertEncoded, CRYPT_STRING_BASE64HEADER, pem.data(), &length))
			throw std::runtime_error("Could not encode local TLS certificate");
		pem.resize(length);
		while (!pem.empty() && pem.back() == '\0')
			pem.pop_back();
		return pem;
	}
}

extern "C" int WSAAPI EvolveGetAddrInfo(PCSTR host, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* result) {
	if (!host)
		return getaddrinfo(host, service, hints, result);
	const std::string text(host);
	if (!offline::IsProfileHost(std::wstring(text.begin(), text.end())))
		return getaddrinfo(host, service, hints, result);
	try {
		const auto port = std::to_string(LocalServer().port);
		ADDRINFOA local{};
		if (hints)
			local = *hints;
		local.ai_family = AF_INET;
		local.ai_socktype = SOCK_STREAM;
		local.ai_protocol = IPPROTO_TCP;
		return getaddrinfo("127.0.0.1", port.c_str(), &local, result);
	} catch (...) {
		return EAI_FAIL;
	}
}

void InstallBackendResolver() {
	const auto image = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
	const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
	const auto pe = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
	const auto directory = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!directory.VirtualAddress)
		return;
	auto descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image + directory.VirtualAddress);
	for (; descriptor->Name; ++descriptor) {
		if (_stricmp(reinterpret_cast<const char*>(image + descriptor->Name), "WS2_32.dll") ||
		    !descriptor->OriginalFirstThunk)
			continue;
		auto name = reinterpret_cast<IMAGE_THUNK_DATA64*>(image + descriptor->OriginalFirstThunk);
		auto slot = reinterpret_cast<IMAGE_THUNK_DATA64*>(image + descriptor->FirstThunk);
		for (; name->u1.AddressOfData; ++name, ++slot) {
			if (IMAGE_SNAP_BY_ORDINAL64(name->u1.Ordinal))
				continue;
			const auto imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(image + name->u1.AddressOfData);
			if (strcmp(reinterpret_cast<const char*>(imported->Name), "getaddrinfo"))
				continue;
			DWORD old = 0;
			if (VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), PAGE_READWRITE, &old)) {
				InterlockedExchangePointer(
				    reinterpret_cast<void* volatile*>(&slot->u1.Function), reinterpret_cast<void*>(&EvolveGetAddrInfo));
				VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), old, &old);
			}
		}
	}
}
