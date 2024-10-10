#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <obs.hpp>
#include <util/windows/ComPtr.hpp>

#include <QScopeGuard>

#include <WbemCli.h>

struct COMUninitializer {
	~COMUninitializer() { CoUninitialize(); }
};

class WMIDataQuery;

class WMIProvider {
public:
	static std::shared_ptr<WMIProvider> CreateProvider();
	bool Refresh();

	static std::optional<WMIDataQuery> AddQuery(std::shared_ptr<WMIProvider> self, const wchar_t *query);

private:
	WMIProvider(ComPtr<IWbemServices> service_, ComPtr<IWbemRefresher> refresher_)
		: service_(service_),
		  refresher_(refresher_)
	{
	}

	COMUninitializer com_uninitializer_;
	ComPtr<IWbemServices> service_;
	ComPtr<IWbemRefresher> refresher_;
};

class WMIDataQuery {
public:
	template<typename Func> bool GetData(Func &&func)
	{
		std::vector<IWbemObjectAccess *> objects;
		if (!LoadObjects(objects))
			return false;

		auto guard = QScopeGuard([&] { ReleaseObjects(objects); });

		for (auto object : objects) {
			func(object);
		}
		return true;
	}

	const wchar_t *query_;

private:
	WMIDataQuery(const wchar_t *query_, std::shared_ptr<WMIProvider> provider_, ComPtr<IWbemHiPerfEnum> enumerator_)
		: query_(query_),
		  provider_(provider_),
		  enumerator_(enumerator_)
	{
	}

	bool LoadObjects(std::vector<IWbemObjectAccess *> &objects);
	void ReleaseObjects(std::vector<IWbemObjectAccess *> &objects);

	std::shared_ptr<WMIProvider> provider_;
	ComPtr<IWbemHiPerfEnum> enumerator_;

	friend class WMIProvider;
};

struct WMIQueries {
public:
	static std::optional<WMIQueries> Create();

	void SummarizeData(obs_data_t *data);

private:
	WMIQueries(std::shared_ptr<WMIProvider> wmi_provider_, std::optional<WMIDataQuery> cpu_usage_,
		   std::optional<WMIDataQuery> gpu_usage_, std::optional<WMIDataQuery> gpu_memory_usage_)
		: wmi_provider_(wmi_provider_),
		  cpu_usage_(cpu_usage_),
		  gpu_usage_(gpu_usage_),
		  gpu_memory_usage_(gpu_memory_usage_)
	{
	}

	std::shared_ptr<WMIProvider> wmi_provider_;
	std::optional<WMIDataQuery> cpu_usage_;
	std::optional<WMIDataQuery> gpu_usage_;
	std::optional<WMIDataQuery> gpu_memory_usage_;
};
