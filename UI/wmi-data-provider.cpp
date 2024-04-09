#include "wmi-data-provider.hpp"

#include <obs.hpp>
#include <util/platform.h>
#include <util/util.hpp>

#include <comdef.h>
#include <Wbemidl.h>

#include <QScopeGuard>

#include <optional>
#include <string>
#include <unordered_map>

#pragma comment(lib, "wbemuuid.lib")

std::shared_ptr<WMIProvider> WMIProvider::CreateProvider()
{
	auto hres = CoInitializeEx(0, COINIT_MULTITHREADED);
	if (FAILED(hres) && hres != RPC_E_CHANGED_MODE) {
		blog(LOG_WARNING,
		     "Failed to initialize COM library. Error code = 0x%x",
		     hres);
		return nullptr;
	}

	QScopeGuard uninitialize_com{[] {
		CoUninitialize();
	}};

	hres = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
				    RPC_C_AUTHN_LEVEL_DEFAULT,
				    RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
				    EOAC_NONE, nullptr);
	if (FAILED(hres) && hres != RPC_E_TOO_LATE) {
		blog(LOG_WARNING,
		     "Failed to initialize security. Error code = 0x%x", hres);
		return nullptr;
	}

	ComPtr<IWbemLocator> locator = nullptr;

	hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
				IID_IWbemLocator, (LPVOID *)&locator);
	if (FAILED(hres)) {
		blog(LOG_WARNING,
		     "Failed to create IWbemLocator object. Err code = 0x%x",
		     hres);
		return nullptr;
	}

	ComPtr<IWbemServices> service = nullptr;
	hres = locator->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr,
				      0, 0, 0, nullptr, &service);
	if (FAILED(hres)) {
		blog(LOG_WARNING, "Could not connect. Error code = 0x%x", hres);
		return nullptr;
	}

	hres = CoSetProxyBlanket(service, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
				 nullptr, RPC_C_AUTHN_LEVEL_CALL,
				 RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
				 EOAC_NONE);
	if (FAILED(hres)) {
		blog(LOG_INFO, "Could not set proxy blanket. Error code = 0x%x",
		     hres);
		return nullptr;
	}

	ComPtr<IWbemRefresher> refresher;
	hres = CoCreateInstance(CLSID_WbemRefresher, NULL, CLSCTX_INPROC_SERVER,
				IID_IWbemRefresher, (void **)&refresher);
	if (FAILED(hres)) {
		blog(LOG_WARNING,
		     "Could not create WMI refresher. Error code = 0x%x", hres);
		return nullptr;
	}

	uninitialize_com.dismiss();

	return std::make_shared<WMIProvider>(
		WMIProvider{std::move(service), std::move(refresher)});
}

bool WMIProvider::Refresh()
{
	auto hres = refresher_->Refresh(0);
	if (FAILED(hres)) {
		blog(LOG_WARNING,
		     "Failed to refresh WMI data. Error code = 0x%x", hres);
	}
	return SUCCEEDED(hres);
}

std::optional<WMIDataQuery>
WMIProvider::AddQuery(std::shared_ptr<WMIProvider> self, const wchar_t *query)
{
	if (!self)
		return std::nullopt;

	ComPtr<IWbemConfigureRefresher> refresher_config;
	auto hres = self->refresher_->QueryInterface(
		IID_IWbemConfigureRefresher, (void **)&refresher_config);
	if (FAILED(hres)) {
		blog(LOG_WARNING,
		     "Failed to get refresher config. Error code = 0x%x", hres);
		return std::nullopt;
	}

	ComPtr<IWbemHiPerfEnum> enumerator;
	long enum_id;
	hres = refresher_config->AddEnum(self->service_, query, 0, nullptr,
					 &enumerator, &enum_id);
	if (FAILED(hres)) {
		blog(LOG_WARNING, "Failed to add query '%S'. Error code = 0x%x",
		     query, hres);
		return std::nullopt;
	}

	return WMIDataQuery{query, self, enumerator};
}

bool WMIDataQuery::LoadObjects(std::vector<IWbemObjectAccess *> &objects)
{
	ULONG returned = 0;
	auto hres = enumerator_->GetObjects(0, 0, objects.data(), &returned);
	if (hres == WBEM_E_BUFFER_TOO_SMALL) {
		objects.resize(returned);
	} else if (FAILED(hres)) {
		blog(LOG_WARNING,
		     "GetObjects query failed for '%S'. Error code = 0x%x",
		     query_, hres);
		return false;
	}

	hres = enumerator_->GetObjects(0, static_cast<ULONG>(objects.size()),
				       objects.data(), &returned);
	if (FAILED(hres)) {
		blog(LOG_WARNING,
		     "GetObjects failed for '%S'. Error code = 0x%x", query_,
		     hres);
		return false;
	}

	return true;
}

void WMIDataQuery::ReleaseObjects(std::vector<IWbemObjectAccess *> &objects)
{
	for (auto &object : objects)
		object->Release();
	objects.clear();
}

std::optional<WMIQueries> WMIQueries::Create()
{
	auto wmi_provider = WMIProvider::CreateProvider();
	if (!wmi_provider)
		return std::nullopt;

	auto cpu_usage = WMIProvider::AddQuery(
		wmi_provider, L"Win32_PerfFormattedData_PerfOS_Processor");
	auto gpu_usage = WMIProvider::AddQuery(
		wmi_provider,
		L"Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine");
	auto gpu_memory_usage = WMIProvider::AddQuery(
		wmi_provider,
		L"Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory");
	if (!cpu_usage && !gpu_usage && !gpu_memory_usage)
		return std::nullopt;

	return WMIQueries{wmi_provider, cpu_usage, gpu_usage, gpu_memory_usage};
}

struct PropertyHandle {
	PropertyHandle(const long handle, const CIMTYPE ty, const wchar_t *name)
		: handle(handle),
		  ty(ty),
		  name(name)
	{
	}
	const long handle;
	const CIMTYPE ty;
	const wchar_t *name;
};

static bool LoadPropertyHandle(std::optional<PropertyHandle> &handle_holder,
			       const wchar_t *query, IWbemObjectAccess *object,
			       const wchar_t *name)
{
	CIMTYPE ty;
	long handle;
	auto hres = object->GetPropertyHandle(name, &ty, &handle);
	if (FAILED(hres)) {
		blog(LOG_WARNING,
		     "Failed to get property handle for '%S' in query '%S'. Error code = 0x%x",
		     name, query, hres);
		return false;
	}
	handle_holder.emplace(handle, ty, name);
	return true;
}

static std::optional<std::wstring>
ReadPropertyStringValue(const wchar_t *query, IWbemObjectAccess *object,
			const PropertyHandle &handle)
{
	std::vector<char> buffer;
	long required_size = 0;
	auto hres = object->ReadPropertyValue(
		handle.handle, static_cast<long>(buffer.size()), &required_size,
		reinterpret_cast<byte *>(buffer.data()));
	if (hres == WBEM_E_BUFFER_TOO_SMALL) {
		buffer.resize(required_size);
	} else if (FAILED(hres)) {
		blog(LOG_WARNING,
		     "Failed to get property value '%S' for query '%S'. Error code = 0x%x",
		     handle.name, query, hres);
		return std::nullopt;
	}

	hres = object->ReadPropertyValue(
		handle.handle, static_cast<long>(buffer.size()), &required_size,
		reinterpret_cast<byte *>(buffer.data()));
	if (FAILED(hres))
		return std::nullopt;
	return {reinterpret_cast<const wchar_t *>(buffer.data())};
}

static std::optional<uint64_t>
ReadPropertyQWORDValue(const wchar_t *query, IWbemObjectAccess *object,
		       const PropertyHandle &handle)
{
	uint64_t value;
	auto hres = object->ReadQWORD(handle.handle, &value);
	if (FAILED(hres)) {
		blog(LOG_WARNING,
		     "Failed to get property QWORD value '%S' for query '%S'. Error code = 0x%x",
		     handle.name, query, hres);
		return std::nullopt;
	}

	return value;
}

static std::optional<std::string> GetLUID(const std::wstring &str)
{
	auto pos = str.find(L"luid_");
	if (pos == str.npos)
		return std::nullopt;

	auto first_underscore = str.find(L"_", pos + 5);
	if (first_underscore == str.npos)
		return std::nullopt;

	auto second_underscore = str.find(L"_", first_underscore + 1);
	if (second_underscore == str.npos)
		return std::nullopt;

	BPtr<char> buffer;
	auto len = os_wcs_to_utf8_ptr(str.c_str() + pos,
				      second_underscore - pos, &buffer);
	return std::string{buffer.Get(), len};
}

template<typename Func>
bool GetData(std::optional<WMIDataQuery> &query, Func &&func)
{
	return query.has_value() ? query->GetData(std::forward<Func>(func))
				 : false;
}

void WMIQueries::SummarizeData(obs_data_t *data)
{
	if (!wmi_provider_->Refresh())
		return;

	{
		// handles seem to stay the same between refresh calls
		static std::optional<PropertyHandle> name_handle;
		static std::optional<PropertyHandle>
			percent_processor_time_handle;
		GetData(cpu_usage_, [&](IWbemObjectAccess *object) {
			if (!name_handle.has_value() &&
			    !LoadPropertyHandle(name_handle, cpu_usage_->query_,
						object, L"Name"))
				return false;
			if (!percent_processor_time_handle.has_value() &&
			    !LoadPropertyHandle(percent_processor_time_handle,
						cpu_usage_->query_, object,
						L"PercentProcessorTime"))
				return false;

			auto name = ReadPropertyStringValue(
				cpu_usage_->query_, object, *name_handle);
			if (name != L"_Total")
				return true;

			auto percent_processor_time = ReadPropertyQWORDValue(
				cpu_usage_->query_, object,
				*percent_processor_time_handle);

			if (percent_processor_time.has_value())
				obs_data_set_int(data, "cpu_usage_percentage",
						 *percent_processor_time);
			return false;
		});
	}

	{
		static std::optional<PropertyHandle> name_handle;
		static std::optional<PropertyHandle>
			utilization_percentage_handle;
		std::unordered_map<std::string, uint64_t> usages;
		GetData(gpu_usage_, [&](IWbemObjectAccess *object) {
			if (!name_handle.has_value() &&
			    !LoadPropertyHandle(name_handle, gpu_usage_->query_,
						object, L"name"))
				return false;
			if (!utilization_percentage_handle.has_value() &&
			    !LoadPropertyHandle(utilization_percentage_handle,
						gpu_usage_->query_, object,
						L"UtilizationPercentage"))
				return false;

			auto name = ReadPropertyStringValue(
				gpu_usage_->query_, object, *name_handle);
			if (!name.has_value())
				return true;

			auto value = ReadPropertyQWORDValue(
				gpu_usage_->query_, object,
				*utilization_percentage_handle);
			if (!value.has_value())
				return true;

			auto luid = GetLUID(*name);
			if (!luid.has_value())
				return true;

			auto engine_name = [&]() -> std::optional<std::string> {
				auto pos = name->rfind(L"engtype_");
				if (pos == name->npos)
					return std::nullopt;

				BPtr<char> data;
				auto len = os_wcs_to_utf8_ptr(
					name->c_str() + pos,
					name->length() - pos, &data);
				return std::string{data.Get(), len};
			}();
			if (!engine_name.has_value())
				return true;

			usages[*luid + "_" + *engine_name] += *value;
			return true;
		});
		OBSDataAutoRelease usages_data = obs_data_create();
		for (const auto &usage : usages) {
			obs_data_set_int(usages_data, usage.first.c_str(),
					 usage.second);
		}
		obs_data_set_string(data, "gpu_usage",
				    obs_data_get_json(usages_data));
	}

	{
		static std::optional<PropertyHandle> name_handle;
		static std::optional<PropertyHandle> dedicated_usage_handle;
		static std::optional<PropertyHandle> shared_usage_handle;
		std::unordered_map<std::string, std::pair<uint64_t, uint64_t>>
			adapter_memory_usages;
		GetData(gpu_memory_usage_, [&](IWbemObjectAccess *object) {
			if (!name_handle.has_value() &&
			    !LoadPropertyHandle(name_handle,
						gpu_memory_usage_->query_,
						object, L"name"))
				return false;
			if (!dedicated_usage_handle.has_value() &&
			    !LoadPropertyHandle(dedicated_usage_handle,
						gpu_memory_usage_->query_,
						object, L"DedicatedUsage"))
				return false;
			if (!shared_usage_handle.has_value() &&
			    !LoadPropertyHandle(shared_usage_handle,
						gpu_memory_usage_->query_,
						object, L"SharedUsage"))
				return false;

			auto name = ReadPropertyStringValue(
				gpu_memory_usage_->query_, object,
				*name_handle);
			if (!name.has_value())
				return true;

			auto value = ReadPropertyQWORDValue(
				gpu_memory_usage_->query_, object,
				*dedicated_usage_handle);
			if (!value.has_value())
				return true;

			auto shared_value = ReadPropertyQWORDValue(
				gpu_memory_usage_->query_, object,
				*shared_usage_handle);
			if (!shared_value.value())
				return true;

			auto luid = GetLUID(*name);
			if (!luid.has_value())
				return true;

			auto &p = adapter_memory_usages[*luid];
			p.first += *value;
			p.second += *shared_value;
			return true;
		});
		OBSDataAutoRelease memory_usages = obs_data_create();
		for (auto &adapter_memory_usage : adapter_memory_usages) {
			obs_data_set_int(
				memory_usages,
				adapter_memory_usage.first.c_str(),
				adapter_memory_usage.second.first -
					adapter_memory_usage.second.second);
		}
		obs_data_set_string(data, "gpu_dedicated_memory_usage",
				    obs_data_get_json(memory_usages));
	}
}
