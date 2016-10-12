/*
Copyright (C) 2016 iNuron NV

This file is part of Open vStorage Open Source Edition (OSE), as available from


    http://www.openvstorage.org and
    http://www.openvstorage.com.

This file is free software; you can redistribute it and/or modify it
under the terms of the GNU Affero General Public License v3 (GNU AGPLv3)
as published by the Free Software Foundation, in version 3 as it comes
in the <LICENSE.txt> file of the Open vStorage OSE distribution.

Open vStorage is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY of any kind.
*/

#include "rora_proxy_client.h"
#include "alba_logger.h"
#include "manifest.h"
#include "manifest_cache.h"
#include "osd_access.h"

namespace alba {
namespace proxy_client {
using std::string;

RoraProxy_client::RoraProxy_client(
    std::unique_ptr<GenericProxy_client> delegate,
    const RoraConfig &rora_config)
    : _delegate(std::move(delegate)), _use_null_io(rora_config.use_null_io) {
  ALBA_LOG(INFO, "RoraProxy_client(...)");
  ManifestCache::set_capacity(rora_config.manifest_cache_size);
}

bool RoraProxy_client::namespace_exists(const string &name) {
  return _delegate->namespace_exists(name);
};

void RoraProxy_client::create_namespace(
    const string &name, const boost::optional<string> &preset_name) {
  _delegate->create_namespace(name, preset_name);
};

void RoraProxy_client::delete_namespace(const string &name) {
  _delegate->delete_namespace(name);
};

std::tuple<std::vector<string>, has_more> RoraProxy_client::list_namespaces(
    const string &first, const include_first include_first_,
    const boost::optional<string> &last, const include_last include_last_,
    const int max, const reverse reverse_) {
  return _delegate->list_namespaces(first, include_first_, last, include_last_,
                                    max, reverse_);
}

void _maybe_add_to_manifest_cache(const string &namespace_,
                                  const string &alba_id,
                                  std::shared_ptr<ManifestWithNamespaceId> mf) {
  using alba::stuff::operator<<;
  if (compressor_t::NO_COMPRESSION == mf->compression->get_compressor() &&
      encryption_t::NO_ENCRYPTION == mf->encrypt_info->get_encryption()) {
    ManifestCache::getInstance().add(namespace_, alba_id, std::move(mf));
  }
}

void RoraProxy_client::write_object_fs(const string &namespace_,
                                       const string &object_name,
                                       const string &input_file,
                                       const allow_overwrite overwrite,
                                       const Checksum *checksum) {
  std::vector<std::shared_ptr<proxy_client::sequences::Assert>> asserts;
  if (overwrite == allow_overwrite::F) {
    asserts.push_back(
        std::make_shared<proxy_client::sequences::AssertObjectDoesNotExist>(
            object_name));
  }

  std::vector<std::shared_ptr<proxy_client::sequences::Update>> updates;
  updates.push_back(
      std::make_shared<proxy_client::sequences::UpdateUploadObjectFromFile>(
          object_name, input_file, checksum));

  apply_sequence(namespace_, write_barrier::F, asserts, updates);
}

void RoraProxy_client::read_object_fs(const string &namespace_,
                                      const string &object_name,
                                      const string &dest_file,
                                      const consistent_read consistent_read_,
                                      const should_cache should_cache_) {
  _delegate->read_object_fs(namespace_, object_name, dest_file,
                            consistent_read_, should_cache_);
}

void RoraProxy_client::delete_object(const string &namespace_,
                                     const string &object_name,
                                     const may_not_exist may_not_exist_) {
  _delegate->delete_object(namespace_, object_name, may_not_exist_);
}

std::tuple<std::vector<string>, has_more> RoraProxy_client::list_objects(
    const string &namespace_, const string &first,
    const include_first include_first_, const boost::optional<string> &last,
    const include_last include_last_, const int max, const reverse reverse_) {
  return _delegate->list_objects(namespace_, first, include_first_, last,
                                 include_last_, max, reverse_);
}

string fragment_key(const uint32_t namespace_id, const string &object_id,
                    uint32_t version_id, uint32_t chunk_id,
                    uint32_t fragment_id) {
  llio::message_builder mb;
  char instance_content_prefix = 'p';
  mb.add_raw(&instance_content_prefix, 1);
  uint32_t zero = 0;
  to(mb, zero);
  char namespace_char = 'n';
  mb.add_raw(&namespace_char, 1);
  llio::to_be(mb, namespace_id);
  char prefix = 'o';
  mb.add_raw(&prefix, 1);
  to(mb, object_id);
  to(mb, chunk_id);
  to(mb, fragment_id);
  to(mb, version_id);
  string r = mb.as_string();
  return r.substr(4, r.size() - 4);
}

void _dump(std::map<osd_t, std::vector<asd_slice>> &per_osd) {
  std::cout << "_dump per_osd.size()=" << per_osd.size();
  for (auto &item : per_osd) {
    osd_t osd = item.first;
    auto &asd_slices = item.second;
    std::cout << "asd_slices.size()=" << asd_slices.size();
    std::cout << osd << ": [";
    for (auto &asd_slice : asd_slices) {

      void *p = asd_slice.target;
      std::cout << "( " << asd_slice.offset << ", " << asd_slice.len << ", "
                << p << "),";
    }
    std::cout << "]," << std::endl;
  }
}

void RoraProxy_client::_maybe_update_osd_infos(
    std::map<osd_t, std::vector<asd_slice>> &per_osd) {

  ALBA_LOG(DEBUG, "RoraProxy_client::_maybe_update_osd_infos(_)");
  bool ok = true;
  auto &access = OsdAccess::getInstance();
  for (auto &item : per_osd) {
    osd_t osd = item.first;
    if (access.osd_is_unknown(osd)) {
      ok = false;
      break;
    }
  }

  if (!ok) {
    ALBA_LOG(DEBUG, "RoraProxy_client:: refresh from proxy");
    access.update(*this);
  }
}

Location get_location(ManifestWithNamespaceId &mf, uint64_t pos, uint32_t len) {
  int chunk_index = -1;
  uint64_t total = 0;

  {
    auto it = mf.chunk_sizes.begin();
    while (total <= pos) {
      chunk_index++;
      auto chunk_size = *it;
      total += chunk_size;
      it++;
    }
  }

  auto &chunk_fragment_locations = mf.fragment_locations[chunk_index];

  uint32_t chunk_size = mf.chunk_sizes[chunk_index];
  total -= chunk_size;
  uint32_t fragment_length = chunk_size / mf.encoding_scheme.k;
  uint32_t pos_in_chunk = pos - total;

  uint32_t fragment_index = pos_in_chunk / fragment_length;
  auto p = chunk_fragment_locations[fragment_index];

  total += fragment_length * fragment_index;
  uint32_t pos_in_fragment = pos - total;

  Location l;
  l.namespace_id = mf.namespace_id;
  l.object_id = mf.object_id;
  l.chunk_id = chunk_index;
  l.fragment_id = fragment_index;
  l.fragment_location = p;
  l.offset = pos_in_chunk;
  l.length = std::min(len, fragment_length - pos_in_fragment);
  return l;
}

void _resolve_slice_one_level(std::vector<std::pair<byte *, Location>> &results,
                              ManifestWithNamespaceId &manifest,
                              uint64_t offset, uint32_t length, byte *target) {
  while (length > 0) {
    results.emplace_back(target, get_location(manifest, offset, length));
    auto &l = results.back().second;
    length -= l.length;
    offset += l.length;
    target += l.length;
  };
}

boost::optional<std::vector<std::pair<byte *, Location>>>
_resolve_one_level(const alba_id_t &alba_id, const std::string &namespace_,
                   const ObjectSlices obj_slices) {
  auto &cache = ManifestCache::getInstance();
  auto mf = cache.find(namespace_, alba_id, obj_slices.object_name);
  if (mf == nullptr) {
    ALBA_LOG(DEBUG, "manifest for alba_id=" << alba_id << ", obj_slices="
                                            << obj_slices << " not found");
    return boost::none;
  } else {
    ALBA_LOG(DEBUG, "manifest for alba_id=" << alba_id << ", obj_slices="
                                            << obj_slices << " found");
    std::vector<std::pair<byte *, Location>> results;
    for (auto &slice : obj_slices.slices) {
      _resolve_slice_one_level(results, *mf, slice.offset, slice.size,
                               slice.buf);
    }
    return results;
  }
}

boost::optional<std::vector<std::pair<byte *, Location>>>
_resolve_one_many_levels(const std::vector<alba_id_t> &alba_levels,
                         const uint alba_level_num,
                         const std::string &namespace_,
                         const ObjectSlices &obj_slices) {
  auto &alba_id = alba_levels[alba_level_num];
  auto locations = _resolve_one_level(alba_id, namespace_, obj_slices);
  if (locations == boost::none) {
    return boost::none;
  } else {
    if ((alba_level_num + 1) < alba_levels.size()) {
      std::vector<std::pair<byte *, Location>> locations_final_level;
      locations_final_level.resize(locations->size());
      for (auto &buf_l : *locations) {
        auto l = std::get<1>(buf_l);
        message_builder mb;
        to(mb, l.object_id);
        to(mb, l.chunk_id);
        to(mb, l.fragment_id);
        SliceDescriptor slice{std::get<0>(buf_l), l.offset, l.length};
        std::vector<SliceDescriptor> slices{slice};

        string r = mb.as_string();
        string fragment_cache_object_name = r.substr(4, r.size() - 4);

        ObjectSlices obj_slices{fragment_cache_object_name, slices};
        ALBA_LOG(DEBUG, "_resolve_one_many_levels: obj_slices=" << obj_slices);

        auto locations = _resolve_one_many_levels(
            alba_levels, alba_level_num + 1, namespace_, obj_slices);
        if (locations == boost::none) {
          return boost::none;
        } else {
          for (auto &l : *locations) {
            locations_final_level.push_back(l);
          }
        }
      }
      return locations_final_level;
    } else {
      return locations;
    }
  }
}

int RoraProxy_client::_short_path(
    const std::vector<std::pair<byte *, Location>> &locations) {

  ALBA_LOG(DEBUG, "_short_path locations.size()=" << locations.size());

  std::map<osd_t, std::vector<asd_slice>> per_osd;

  for (auto &bl : locations) {
    auto &target = std::get<0>(bl);
    auto &l = std::get<1>(bl);

    osd_t osd_id = *std::get<0>(*l.fragment_location);
    uint32_t version_id = std::get<1>(*l.fragment_location);

    asd_slice slice;
    slice.offset = l.offset;
    slice.len = l.length;
    slice.target = target;
    string key = fragment_key(l.namespace_id, l.object_id, version_id,
                              l.chunk_id, l.fragment_id);
    slice.key = key;

    auto it = per_osd.find(osd_id);
    if (it == per_osd.end()) {
      std::vector<asd_slice> slices;
      per_osd[osd_id] = slices;
      it = per_osd.find(osd_id);
    }
    auto &slices = it->second;
    slices.push_back(slice);
  }

  // everything to read is now nicely sorted per osd.
  _maybe_update_osd_infos(per_osd);
  //_dump(per_osd);
  if (_use_null_io) {
    return 0;
  } else {
    return OsdAccess::getInstance().read_osds_slices(per_osd);
  }
}

void RoraProxy_client::_process(std::vector<object_info> &object_infos,
                                const string &namespace_) {

  ALBA_LOG(DEBUG, "_process : " << object_infos.size());
  for (auto &object_info : object_infos) {
    using alba::stuff::operator<<;

    manifest_cache_entry manifest_cache_entry_ =
        std::shared_ptr<alba::proxy_protocol::ManifestWithNamespaceId>(
            std::get<2>(object_info).release());
    string alba_id = std::get<1>(object_info);
    if (alba_id == "") {
      alba_id = OsdAccess::getInstance().get_alba_levels(*this)[0];
    }
    _maybe_add_to_manifest_cache(namespace_, alba_id, manifest_cache_entry_);
  }
}

void RoraProxy_client::read_objects_slices(
    const string &namespace_, const std::vector<ObjectSlices> &slices,
    const consistent_read consistent_read_) {

  if (consistent_read_ == consistent_read::T) {
    std::vector<object_info> object_infos;
    _delegate->read_objects_slices2(namespace_, slices, consistent_read_,
                                    object_infos);
    _process(object_infos, namespace_);
  } else {
    std::vector<std::pair<byte *, Location>> short_path;
    std::vector<ObjectSlices> via_proxy;
    auto alba_levels = OsdAccess::getInstance().get_alba_levels(*this);
    for (auto &object_slices : slices) {
      auto locations =
          _resolve_one_many_levels(alba_levels, 0, namespace_, object_slices);
      if (locations == boost::none ||
          std::any_of(locations->begin(), locations->end(),
                      [](std::pair<byte *, Location> &l) {
                        return std::get<1>(l).fragment_location == boost::none;
                      })) {
        via_proxy.push_back(object_slices);
      } else {
        for (auto &l : *locations) {
          short_path.push_back(l);
        }
      }
    }

    // TODO: different paths could go in parallel
    int result_front = _short_path(short_path);
    ALBA_LOG(DEBUG, "_short_path result => " << result_front);

    if (result_front) {
      via_proxy.clear();
      for (auto &s : slices) {
        via_proxy.push_back(s);
      }
    }

    ALBA_LOG(DEBUG, "rora read_objects_slices going via proxy, size="
                        << via_proxy.size());
    std::vector<object_info> object_infos;
    _delegate->read_objects_slices2(namespace_, via_proxy, consistent_read_,
                                    object_infos);
    _process(object_infos, namespace_);
  }
}

std::tuple<uint64_t, Checksum *> RoraProxy_client::get_object_info(
    const string &namespace_, const string &object_name,
    const consistent_read consistent_read_, const should_cache should_cache_) {
  return _delegate->get_object_info(namespace_, object_name, consistent_read_,
                                    should_cache_);
}

void RoraProxy_client::apply_sequence(
    const std::string &namespace_, const write_barrier write_barrier,
    const std::vector<std::shared_ptr<sequences::Assert>> &asserts,
    const std::vector<std::shared_ptr<sequences::Update>> &updates) {
  std::vector<proxy_protocol::object_info> object_infos;
  _delegate->apply_sequence_(namespace_, write_barrier, asserts, updates,
                             object_infos);

  _process(object_infos, namespace_);
}

void RoraProxy_client::invalidate_cache(const std::string &namespace_) {
  ManifestCache::getInstance().invalidate_namespace(namespace_);
  _delegate->invalidate_cache(namespace_);
}

void RoraProxy_client::drop_cache(const string &namespace_) {
  _delegate->drop_cache(namespace_);
}

std::tuple<int32_t, int32_t, int32_t, string>
RoraProxy_client::get_proxy_version() {
  return _delegate->get_proxy_version();
}

double RoraProxy_client::ping(const double delay) {
  return _delegate->ping(delay);
}

void RoraProxy_client::osd_info(osd_map_t &result) {
  ALBA_LOG(DEBUG, "RoraProxy_client::osd_info");
  _delegate->osd_info(result);
}

void RoraProxy_client::osd_info2(osd_maps_t &result) {
  ALBA_LOG(DEBUG, "RoraProxy_client::osd_info2");
  _delegate->osd_info2(result);
}
}
}
