(*
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
*)

open Prelude
open Lwt_bytes2
open Slice
open Alba_statistics
open Alba_client_errors
open Lwt.Infix


let get_object_manifests'
      (nsm_host_access : Nsm_host_access.nsm_host_access)
      manifest_cache
      ~namespace_id ~object_names
      ~consistent_read ~should_cache =
  Lwt_log.debug_f
    "get_object_manifest %Li %S ~consistent_read:%b ~should_cache:%b"
    namespace_id
    ([%show: string list] object_names)
    consistent_read
    should_cache
  >>= fun () ->
  let lookup_on_nsm_host object_names =
    nsm_host_access # get_nsm_by_id ~namespace_id >>= fun client ->
    client # get_object_manifests_by_name object_names
  in
  Manifest_cache.ManifestCache.lookup_multiple
    manifest_cache
    namespace_id object_names
    lookup_on_nsm_host
    ~consistent_read ~should_cache

let get_object_manifest'
      nsm_host_access
      manifest_cache
      ~namespace_id ~object_name
      ~consistent_read ~should_cache =
  get_object_manifests'
    nsm_host_access
    manifest_cache
    ~namespace_id ~object_names:[ object_name; ]
    ~consistent_read ~should_cache >>= function
  | [ x ] -> Lwt.return x
  | _ -> assert false


module E = Prelude.Error.Lwt
let (>>==) = E.bind

(* consumers of this method are responsible for freeing
 * the returned fragment bigstring
 *)
let download_packed_fragment
      (osd_access : Osd_access_type.t)
      ~(location:Nsm_model.osd_id * Nsm_model.version)
      ~namespace_id
      ~object_id ~object_name
      ~chunk_id ~fragment_id
  =

  let osd_id, version_id = location in

  Lwt_log.debug_f
    "download_packed_fragment: object (%S, %S) chunk %i, fragment %i from osd_id:%Li"
    object_id object_name
    chunk_id fragment_id
    osd_id
  >>= fun () ->

  let osd_key =
    Osd_keys.AlbaInstance.fragment
      ~object_id ~version_id
      ~chunk_id ~fragment_id
    |> Slice.wrap_string
  in

  Lwt.catch
    (fun () ->
      Lwt_extra2.with_timeout
        ~msg:"download_packed_fragment"
        (osd_access # osd_timeout)
        (fun () ->
          osd_access # with_osd
                     ~osd_id
                     (fun device_client ->
                       (device_client # namespace_kvs namespace_id)
                         # get_option
                         (osd_access # get_default_osd_priority)
                         osd_key
                       >>= E.return))
    )
    (let open Asd_protocol.Protocol in
     function
     | Error.Exn err -> E.fail (`AsdError err)
     | exn -> E.fail (`AsdExn exn)
    )
  >>== function
  | None ->
     E.fail `FragmentMissing
  | Some data ->
     osd_access # get_osd_info ~osd_id >>= fun (_, state,_) ->
     Osd_state.add_read state;
     E.return (osd_id, data)

(* consumers of this method are responsible for freeing
 * the returned fragment bigstring
 *)
let download_fragment
      (osd_access : Osd_access_type.t)
      ~location
      ~namespace_id
      ~object_id ~object_name
      ~chunk_id ~fragment_id
      ~k
      ~fragment_checksum
      ~fragment_ctr
      decompress
      ~encryption
      (fragment_cache : Fragment_cache.cache)
      ~cache_on_read
  =

  let t0_fragment = Unix.gettimeofday () in

  let cache_key =
    Fragment_cache_keys.make_key
      ~object_id
      ~chunk_id
      ~fragment_id
  in
  let fc_timeout  = osd_access # osd_timeout *. 0.5 in
  fragment_cache # lookup
                 ~timeout:fc_timeout namespace_id cache_key
  >>= function
  | Some (sb, mfs) ->
     E.return (Statistics.FromCache (Unix.gettimeofday () -. t0_fragment),
               sb, mfs)
  | None ->
     let download_and_unpack () =
       E.with_timing
         (fun () ->
           download_packed_fragment
             osd_access
             ~location
             ~namespace_id
             ~object_id ~object_name
             ~chunk_id ~fragment_id)
       >>== fun (t_retrieve, (osd_id, fragment_data)) ->

       E.with_timing
         (fun () ->
           Fragment_helper.verify fragment_data fragment_checksum
           >>= E.return)
       >>== fun (t_verify, checksum_valid) ->

       (if checksum_valid
        then E.return ()
        else
          begin
            Lwt_bytes.unsafe_destroy fragment_data;
            osd_access # get_osd_info ~osd_id >>= fun (_,osd_state,_) ->
            Osd_state.add_checksum_errors osd_state 1L;
            E.fail `ChecksumMismatch
          end) >>== fun () ->

       E.with_timing
         (fun () ->
           Fragment_helper.maybe_decrypt
             encryption
             ~object_id ~chunk_id ~fragment_id
             ~ignore_fragment_id:(k=1)
             fragment_data
             ~fragment_ctr
           >>= E.return)
       >>== fun (t_decrypt, maybe_decrypted) ->

       E.with_timing
         (fun () ->
           decompress maybe_decrypted
           >>= E.return)
       >>== fun (t_decompress, (maybe_decompressed : Lwt_bytes.t)) ->
       let shared = SharedBuffer.make_shared maybe_decompressed in
       let () =
         if cache_on_read && fragment_id < k (* only cache data fragments *)
         then
           let () = SharedBuffer.register_sharing shared in
           let t () =
             Lwt.finalize
               (fun () ->
                 fragment_cache # add
                                namespace_id
                                cache_key
                                (Bigstring_slice.wrap_shared_buffer shared)
                 >>= fun _mfs ->
                 Lwt.return_unit)
               (fun () ->
                 let () = SharedBuffer.unregister_usage shared in
                 Lwt.return_unit)
           in
           Lwt.async t
       in

       let t_fragment = Statistics.({
                                       osd_id;
                                       retrieve = t_retrieve;
                                       verify = t_verify;
                                       decrypt = t_decrypt;
                                       decompress = t_decompress;
                                       total = Unix.gettimeofday () -. t0_fragment;
                        })
       in
       let mfs = [] in
       E.return (t_fragment, shared, mfs)
     in

     let download_fragment_dedup_cache = osd_access # get_download_fragment_dedup_cache in

     let dedup_key = location, namespace_id, object_id, chunk_id, fragment_id in
     match Hashtbl.find_option download_fragment_dedup_cache dedup_key with
     | Some us ->
        let t, u = Lwt.wait () in
        Hashtbl.replace download_fragment_dedup_cache dedup_key (u::us);
        t
     | None ->
        Hashtbl.add
          download_fragment_dedup_cache
          dedup_key
          [];
        Lwt.catch
          (fun () ->
            download_and_unpack () >>= fun r ->
            let wakers = Hashtbl.find download_fragment_dedup_cache dedup_key in
            Hashtbl.remove download_fragment_dedup_cache dedup_key;

            let r' = match r with
              | Prelude.Error.Error _ as r -> r
              | Prelude.Error.Ok (t_fragment, b, mfs) ->
                 Lwt_bytes2.SharedBuffer.register_sharing ~n:(List.length wakers) b;
                 Prelude.Error.Ok (Statistics.FromOsd (t_fragment, wakers <> []), b, mfs)
            in

            List.iter
              (fun u -> Lwt.wakeup u r')
              wakers;

            Lwt.return r'
          )
          (fun exn ->
            let wakers = Hashtbl.find download_fragment_dedup_cache dedup_key in
            Hashtbl.remove download_fragment_dedup_cache dedup_key;
            List.iter
              (fun u -> Lwt.wakeup_exn u exn)
              wakers;
            Lwt.fail exn
          )

(* consumers of this method are responsible for freeing
 * the returned fragment bigstring
 *)
let download_fragment'
      osd_access
      ~location
      ~namespace_id
      ~object_id ~object_name
      ~chunk_id ~fragment_id
      ~k
      ~fragment_checksum
      ~fragment_ctr
      decompress
      ~encryption
      fragment_cache
      ~cache_on_read
      bad_fragment_callback
  =
  download_fragment
    osd_access
    ~location
    ~namespace_id
    ~object_id ~object_name
    ~chunk_id ~fragment_id
    ~k
    ~fragment_checksum
    ~fragment_ctr
    decompress
    ~encryption
    fragment_cache
    ~cache_on_read
  >>= function
  | Prelude.Error.Ok a -> Lwt.return a
  | Prelude.Error.Error x ->
     let () =
       match bad_fragment_callback
       with | None -> ()
            | Some bfc ->
               bfc ~namespace_id ~object_name ~object_id
                   ~chunk_id ~fragment_id ~location
     in
     match x with
     | `AsdError err -> Lwt.fail (Asd_protocol.Protocol.Error.Exn err)
     | `AsdExn exn -> Lwt.fail exn
     | `FragmentMissing -> Lwt.fail_with "missing fragment"
     | `ChecksumMismatch -> Lwt.fail_with "checksum mismatch"


type download_strategy =
  | AllFragments
  | LeastAmount
[@@deriving show]

(* consumers of this method are responsible for freeing
 * the returned fragment bigstrings
 *)
let download_chunk
      ?(download_strategy = AllFragments)
      ~namespace_id
      ~object_id ~object_name
      chunk_locations ~chunk_id
      decompress
      ~encryption
      k m w'
      (osd_access:Osd_access_type.t)
      fragment_cache
      ~cache_on_read
      bad_fragment_callback
      ~(read_preference: string list)
  =

  let t0_chunk = Unix.gettimeofday () in

  let n = k + m in
  let fragments = Hashtbl.create n in

  let module CountDownLatch = Lwt_extra2.CountDownLatch in
  let downloadable_chunk_locations_i, nones =
    Alba_client_common.downloadable chunk_locations
  in
  begin
    Lwt_log.debug_f "download_strategy:%s"
                  (show_download_strategy download_strategy) >>= fun () ->
    match download_strategy with
    | AllFragments -> Lwt.return (downloadable_chunk_locations_i, k, m+1 - nones)
    | LeastAmount  ->
       Alba_client_common.sort_by_preference
         read_preference osd_access downloadable_chunk_locations_i
       >>= fun sorted ->
       Lwt.return (List.take k sorted , k , 1)
  end
  >>= fun (chunk_locations_i', success_count, failure_count) ->

  let successes = CountDownLatch.create ~count:success_count in
  let failures = CountDownLatch.create ~count:failure_count  in
  let finito = ref false in

  let threads : unit Lwt.t list =
    List.map
      (fun (fragment_id, (location, fragment_checksum, fragment_ctr)) ->
        let t =
          Lwt.catch
            (fun () ->
              download_fragment'
                osd_access
                ~namespace_id
                ~location
                ~object_id
                ~object_name
                ~chunk_id
                ~fragment_id
                ~k
                ~fragment_checksum
                ~fragment_ctr
                decompress
                ~encryption
                fragment_cache
                ~cache_on_read
                bad_fragment_callback
              >>= fun (t_fragment, fragment_data, _mfs) ->
              let r = t_fragment, fragment_data in

              if !finito
              then
                SharedBuffer.unregister_usage fragment_data
              else
                begin
                  Hashtbl.add fragments fragment_id r;
                  CountDownLatch.count_down successes;
                end;
              Lwt.return ())
            (fun exn ->
              Lwt_log.debug_f
                ~exn
                "Downloading fragment %i failed"
                fragment_id >>= fun () ->
              CountDownLatch.count_down failures;
              Lwt.return ())
        in
        Lwt.ignore_result t;
        t)
      chunk_locations_i'
  in

  ignore threads;

  Lwt.choose [ CountDownLatch.await successes;
               CountDownLatch.await failures; ] >>= fun () ->

  finito := true;

  let () =
    if Hashtbl.length fragments < k
    then
      let () =
        Lwt_log.ign_warning_f
          "could not receive enough fragments for namespace %Li, object %S (%S) chunk %i; got %i while %i needed"
          namespace_id
          object_name object_id
          chunk_id (Hashtbl.length fragments) k
      in
      Hashtbl.iter
        (fun _ (_, fragment) -> SharedBuffer.unregister_usage fragment)
        fragments;

      Error.failwith Error.NotEnoughFragments
  in
  let fragment_size =
    let _, (_, bs) = Hashtbl.choose_first fragments |> Option.get_some in
    SharedBuffer.length bs
  in

  let rec gather_fragments end_fragment acc_fragments erasures cnt = function
    | fragment_id when fragment_id = end_fragment -> acc_fragments, erasures, cnt
    | fragment_id ->
       let fragment_bigarray, erasures', cnt' =
         if Hashtbl.mem fragments fragment_id
         then
           snd (Hashtbl.find fragments fragment_id), erasures, cnt + 1
         else
           let sb = SharedBuffer.create fragment_size in
           sb, fragment_id :: erasures, cnt
       in
       if SharedBuffer.length fragment_bigarray <> fragment_size
       then failwith (Printf.sprintf "fragment %i,%i has size %i while %i expected\n%!" chunk_id fragment_id (SharedBuffer.length fragment_bigarray) fragment_size);
       gather_fragments
         end_fragment
         (fragment_bigarray :: acc_fragments)
         erasures'
         cnt'
         (fragment_id + 1) in

  let t0_gather_decode = Unix.gettimeofday () in
  let data_fragments_rev, erasures_rev, cnt = gather_fragments k [] [] 0 0 in
  let coding_fragments_rev, erasures_rev', cnt = gather_fragments n [] erasures_rev cnt k in

  let data_fragments = List.rev data_fragments_rev in
  let coding_fragments = List.rev coding_fragments_rev in


  let erasures = List.rev (-1 :: erasures_rev') in

  Lwt_log.ign_debug_f
    "erasures = %s"
    ([%show: int list] erasures);

  Erasure.decode
    ~k ~m ~w:w'
    erasures
    data_fragments
    coding_fragments
    fragment_size >>= fun () ->

  let t_now = Unix.gettimeofday () in

  let t_fragments =
    Hashtbl.fold
      (fun _ (t_fragment,_) acc ->
        t_fragment :: acc)
      fragments
      []
  in

  let t_chunk = Statistics.({
                               gather_decode = t_now -. t0_gather_decode;
                               total = t_now -. t0_chunk;
                               fragments = t_fragments;
                }) in

  Lwt.return (data_fragments, coding_fragments, t_chunk)
