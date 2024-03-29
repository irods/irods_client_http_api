#include "irods/private/http_api/process_stash.hpp"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <iterator>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace
{
	// A mapping containing handles to heterogenous objects.
	std::unordered_map<std::string, boost::any> g_stash; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

	// A mutex which protects the map from data corruption.
	std::shared_mutex g_mtx; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables, cert-err58-cpp)

	auto generate_unique_key() -> std::string
	{
		std::string uuid;
		constexpr auto uuid_length{36};
		uuid.reserve(uuid_length);
		uuid = to_string(boost::uuids::random_generator{}());

		while (g_stash.find(uuid) != std::end(g_stash)) {
			uuid = to_string(boost::uuids::random_generator{}());
		}

		return uuid;
	} // generate_unique_key
} // anonymous namespace

namespace irods::http::process_stash
{
	auto insert(boost::any _value) -> std::string
	{
		std::lock_guard lock{g_mtx};
		return g_stash.insert_or_assign(generate_unique_key(), std::move(_value)).first->first;
	} // insert

	auto find(const std::string& _key) -> std::optional<boost::any>
	{
		{
			std::shared_lock lock{g_mtx};
			if (auto iter = g_stash.find(_key); iter != std::end(g_stash)) {
				return iter->second;
			}
		}

		return std::nullopt;
	} // find

	auto erase(const std::string& _key) -> bool
	{
		std::lock_guard lock{g_mtx};
		return g_stash.erase(_key);
	} // erase

	auto erase_if(const std::function<bool(const std::string&, const boost::any&)>& _pred) -> std::size_t
	{
		std::lock_guard lock{g_mtx};
		return std::erase_if(g_stash, [&_pred](const auto& _item) {
			const auto& [k, v] = _item;
			return _pred(k, v);
		});
	} // erase_if

	auto handles() -> std::vector<std::string>
	{
		std::vector<std::string> handles;

		{
			std::shared_lock lock{g_mtx};
			handles.reserve(g_stash.size());
			for (const auto& [k, v] : g_stash) {
				handles.push_back(k);
			}
		}

		return handles;
	} // handles
} // namespace irods::http::process_stash
