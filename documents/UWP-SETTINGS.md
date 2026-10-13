# Configurações do frontend UWP

Inventário extraído pela API da DLL: 167 opções em oito grupos, 135 editáveis e 32 excluídas neste perfil. Valores excluídos são preservados.

O frontend usa o mesmo armazenamento de configurações e perfis por jogo do desktop. O renderer padrão é Vulkan/Dozen; o ciclo de boot brokered está descrito em [BOOT-UWP.md](BOOT-UWP.md). A apresentação permanece em FIFO. Edições e operações sobre os mounts ficam desabilitadas durante a sessão.

| Grupo | Chave desktop | Estado no UWP | Motivo |
| --- | --- | --- | --- |
| General | `addon_install_dir` | Excluída | O UWP usa pastas brokered e armazenamento privado; use Biblioteca/Importar. |
| General | `big_picture_folder_depth` | Editável | Mesmo valor e persistência do desktop |
| General | `big_picture_scale` | Editável | Mesmo valor e persistência do desktop |
| General | `console_language` | Editável | Mesmo valor e persistência do desktop |
| General | `dev_kit_mode` | Editável | Mesmo valor e persistência do desktop |
| General | `discord_rpc_enabled` | Excluída | Discord RPC não está integrado ao AppContainer. |
| General | `extra_dmem_in_mbytes` | Editável | Mesmo valor e persistência do desktop |
| General | `extra_fmem_in_mbytes` | Editável | Mesmo valor e persistência do desktop |
| General | `font_dir` | Excluída | O UWP usa pastas brokered e armazenamento privado; use Biblioteca/Importar. |
| General | `home_dir` | Excluída | O UWP usa pastas brokered e armazenamento privado; use Biblioteca/Importar. |
| General | `install_dirs` | Excluída | O UWP usa pastas brokered e armazenamento privado; use Biblioteca/Importar. |
| General | `neo_mode` | Editável | Mesmo valor e persistência do desktop |
| General | `redzone_patches` | Editável | Mesmo valor e persistência do desktop |
| General | `show_fps_counter` | Editável | Mesmo valor e persistência do desktop |
| General | `show_splash` | Excluída | A apresentação de splash/troféus guest ainda não tem adaptador UWP. |
| General | `sys_modules_dir` | Excluída | O UWP usa pastas brokered e armazenamento privado; use Biblioteca/Importar. |
| General | `trophy_notification_duration` | Excluída | A apresentação de splash/troféus guest ainda não tem adaptador UWP. |
| General | `trophy_notification_side` | Excluída | A apresentação de splash/troféus guest ainda não tem adaptador UWP. |
| General | `trophy_popup_disabled` | Excluída | A apresentação de splash/troféus guest ainda não tem adaptador UWP. |
| General | `volume_slider` | Editável | Mesmo valor e persistência do desktop |
| Network | `connected_to_network` | Editável | Mesmo valor e persistência do desktop |
| Network | `disable_https` | Editável | Mesmo valor e persistência do desktop |
| Network | `enable_upnp` | Excluída | A descoberta/configuração UPnP desktop não tem adaptador AppContainer. |
| Network | `p2p_port` | Editável | Mesmo valor e persistência do desktop |
| Network | `shad_net_enabled` | Editável | Mesmo valor e persistência do desktop |
| Network | `shadnet_server` | Editável | Mesmo valor e persistência do desktop |
| Network | `shadnet_webapi_server` | Editável | Mesmo valor e persistência do desktop |
| Network | `signaling_info` | Editável | Mesmo valor e persistência do desktop |
| Log | `append` | Editável | Mesmo valor e persistência do desktop |
| Log | `enable` | Editável | Mesmo valor e persistência do desktop |
| Log | `filter` | Editável | Mesmo valor e persistência do desktop |
| Log | `flush_level` | Editável | Mesmo valor e persistência do desktop |
| Log | `max_skip_duration` | Editável | Mesmo valor e persistência do desktop |
| Log | `separate` | Editável | Mesmo valor e persistência do desktop |
| Log | `size_limit` | Editável | Mesmo valor e persistência do desktop |
| Log | `skip_duplicate` | Editável | Mesmo valor e persistência do desktop |
| Log | `sync` | Editável | Mesmo valor e persistência do desktop |
| Log | `type` | Excluída | O host usa callback e arquivo, sem console desktop. |
| Debug | `config_version` | Excluída | Metadado interno, não é uma preferência. |
| Debug | `debug_dump` | Editável | Mesmo valor e persistência do desktop |
| Debug | `shader_collect` | Editável | Mesmo valor e persistência do desktop |
| Input | `background_controller_input` | Excluída | Ponteiro, foco e suspensão são controlados pelo host UWP. |
| Input | `camera_id` | Excluída | USB/câmera desktop não têm adaptador brokered neste host. |
| Input | `cursor_hide_timeout` | Excluída | Ponteiro, foco e suspensão são controlados pelo host UWP. |
| Input | `cursor_state` | Excluída | Ponteiro, foco e suspensão são controlados pelo host UWP. |
| Input | `default_controller_id` | Editável | Mesmo valor e persistência do desktop |
| Input | `gyro_invert_roll` | Excluída | O backend SDL WinRT não fornece sensores de movimento neste host. |
| Input | `gyro_invert_x` | Excluída | O backend SDL WinRT não fornece sensores de movimento neste host. |
| Input | `gyro_invert_yaw` | Excluída | O backend SDL WinRT não fornece sensores de movimento neste host. |
| Input | `gyro_swap_yaw_roll` | Excluída | O backend SDL WinRT não fornece sensores de movimento neste host. |
| Input | `ime_accessibility_enabled` | Editável | Mesmo valor e persistência do desktop |
| Input | `ime_url_mail_short_panel` | Editável | Mesmo valor e persistência do desktop |
| Input | `is_circle_enter` | Editável | Mesmo valor e persistência do desktop |
| Input | `motion_controls_enabled` | Excluída | O backend SDL WinRT não fornece sensores de movimento neste host. |
| Input | `special_pad_class` | Editável | Mesmo valor e persistência do desktop |
| Input | `usb_device_backend` | Excluída | USB/câmera desktop não têm adaptador brokered neste host. |
| Input | `use_keyboard_as_keyboard` | Editável | Mesmo valor e persistência do desktop |
| Input | `use_mice_as_mice` | Editável | Mesmo valor e persistência do desktop |
| Input | `use_special_pad` | Editável | Mesmo valor e persistência do desktop |
| Input | `use_unified_input_config` | Editável | Mesmo valor e persistência do desktop |
| Audio | `audio_backend` | Editável | Mesmo valor e persistência do desktop |
| Audio | `openal_hrtf` | Editável | Mesmo valor e persistência do desktop |
| Audio | `openal_main_output_device` | Editável | Mesmo valor e persistência do desktop |
| Audio | `openal_mic_device` | Editável | Mesmo valor e persistência do desktop |
| Audio | `openal_output_mode` | Editável | Mesmo valor e persistência do desktop |
| Audio | `openal_padSpk_output_device` | Editável | Mesmo valor e persistência do desktop |
| Audio | `sdl_main_output_device` | Editável | Mesmo valor e persistência do desktop |
| Audio | `sdl_mic_device` | Editável | Mesmo valor e persistência do desktop |
| Audio | `sdl_padSpk_output_device` | Editável | Mesmo valor e persistência do desktop |
| GPU | `adaptive_skipcaches_mode` | Editável | Mesmo valor e persistência do desktop |
| GPU | `backing_write_memo` | Editável | Mesmo valor e persistência do desktop |
| GPU | `bind_image_lean` | Editável | Mesmo valor e persistência do desktop |
| GPU | `bind_line_prefetch` | Editável | Mesmo valor e persistência do desktop |
| GPU | `bind_noop_memo` | Editável | Mesmo valor e persistência do desktop |
| GPU | `bind_write_plan` | Editável | Mesmo valor e persistência do desktop |
| GPU | `br_mem_fast_state` | Editável | Mesmo valor e persistência do desktop |
| GPU | `clean_sync_peek` | Editável | Mesmo valor e persistência do desktop |
| GPU | `copy_gpu_buffers` | Editável | Mesmo valor e persistência do desktop |
| GPU | `covered_range_skip` | Editável | Mesmo valor e persistência do desktop |
| GPU | `desc_delta_flat` | Editável | Mesmo valor e persistência do desktop |
| GPU | `desc_delta_inplace` | Editável | Mesmo valor e persistência do desktop |
| GPU | `desc_delta_partial` | Editável | Mesmo valor e persistência do desktop |
| GPU | `desc_heap_recycle` | Editável | Mesmo valor e persistência do desktop |
| GPU | `desc_layout_share` | Editável | Mesmo valor e persistência do desktop |
| GPU | `direct_memory_access_enabled` | Excluída | O Dozen usa descritores com limites; acesso BDA direto não está habilitado neste caminho. |
| GPU | `draw_glue_memo` | Editável | Mesmo valor e persistência do desktop |
| GPU | `dump_shaders` | Editável | Mesmo valor e persistência do desktop |
| GPU | `dyn_state_memo` | Editável | Mesmo valor e persistência do desktop |
| GPU | `dyn_state_stamp` | Editável | Mesmo valor e persistência do desktop |
| GPU | `findimg_memo_entries` | Editável | Mesmo valor e persistência do desktop |
| GPU | `findimg_memo_first` | Editável | Mesmo valor e persistência do desktop |
| GPU | `findimg_memo_ways` | Editável | Mesmo valor e persistência do desktop |
| GPU | `findimg_range_invalidate` | Editável | Mesmo valor e persistência do desktop |
| GPU | `findimg_slot_hint` | Editável | Mesmo valor e persistência do desktop |
| GPU | `findimg_trust_gen` | Editável | Mesmo valor e persistência do desktop |
| GPU | `flush_draw_interval` | Editável | Mesmo valor e persistência do desktop |
| GPU | `fsr_enabled` | Editável | Mesmo valor e persistência do desktop |
| GPU | `full_screen` | Editável | Mesmo valor e persistência do desktop |
| GPU | `full_screen_mode` | Editável | Mesmo valor e persistência do desktop |
| GPU | `gather_input_memo` | Editável | Mesmo valor e persistência do desktop |
| GPU | `gpu_thread_core_reserve` | Editável | Mesmo valor e persistência do desktop |
| GPU | `guest_copy_hold_segment` | Editável | Mesmo valor e persistência do desktop |
| GPU | `guest_copy_lock_batch` | Editável | Mesmo valor e persistência do desktop |
| GPU | `hdr_allowed` | Editável | Mesmo valor e persistência do desktop |
| GPU | `image_fast_state` | Editável | Mesmo valor e persistência do desktop |
| GPU | `image_update_direct` | Editável | Mesmo valor e persistência do desktop |
| GPU | `inline_fetch_shader` | Editável | Mesmo valor e persistência do desktop |
| GPU | `internal_screen_height` | Editável | Mesmo valor e persistência do desktop |
| GPU | `internal_screen_width` | Editável | Mesmo valor e persistência do desktop |
| GPU | `key_reuse_hash_diff` | Editável | Mesmo valor e persistência do desktop |
| GPU | `null_gpu` | Editável | Mesmo valor e persistência do desktop |
| GPU | `occlude_all` | Editável | Mesmo valor e persistência do desktop |
| GPU | `one_thread_per_core` | Editável | Mesmo valor e persistência do desktop |
| GPU | `parser_reg_run` | Editável | Mesmo valor e persistência do desktop |
| GPU | `patch_shaders` | Editável | Mesmo valor e persistência do desktop |
| GPU | `pending_pop_throttle` | Editável | Mesmo valor e persistência do desktop |
| GPU | `pipeline_key_stamp_reuse` | Editável | Mesmo valor e persistência do desktop |
| GPU | `present_mode` | Excluída | O host apresenta todos os frames em FIFO, sem descarte. |
| GPU | `push_const_dedup` | Editável | Mesmo valor e persistência do desktop |
| GPU | `push_desc_full_limit` | Editável | Mesmo valor e persistência do desktop |
| GPU | `push_vp_memo` | Editável | Mesmo valor e persistência do desktop |
| GPU | `rcas_attenuation` | Editável | Mesmo valor e persistência do desktop |
| GPU | `rcas_enabled` | Editável | Mesmo valor e persistência do desktop |
| GPU | `readback_linear_images_async` | Editável | Mesmo valor e persistência do desktop |
| GPU | `readback_linear_images_enabled` | Editável | Mesmo valor e persistência do desktop |
| GPU | `readback_offload` | Editável | Mesmo valor e persistência do desktop |
| GPU | `readbacks_mode` | Editável | Mesmo valor e persistência do desktop |
| GPU | `residency_bitmap` | Editável | Mesmo valor e persistência do desktop |
| GPU | `ri_memo_fused_cmp` | Editável | Mesmo valor e persistência do desktop |
| GPU | `ring_drain_flush_draws` | Editável | Mesmo valor e persistência do desktop |
| GPU | `rt_state_stamp` | Editável | Mesmo valor e persistência do desktop |
| GPU | `runtime_info_input_memo` | Editável | Mesmo valor e persistência do desktop |
| GPU | `runtime_info_stamp_gate` | Editável | Mesmo valor e persistência do desktop |
| GPU | `shader_params_memo` | Editável | Mesmo valor e persistência do desktop |
| GPU | `shader_params_memo_entries` | Editável | Mesmo valor e persistência do desktop |
| GPU | `spec_fp_cache` | Editável | Mesmo valor e persistência do desktop |
| GPU | `spec_fp_canonical` | Editável | Mesmo valor e persistência do desktop |
| GPU | `spec_fp_front` | Editável | Mesmo valor e persistência do desktop |
| GPU | `spec_fp_slot_inplace` | Editável | Mesmo valor e persistência do desktop |
| GPU | `spec_key_fast` | Editável | Mesmo valor e persistência do desktop |
| GPU | `spec_key_fused` | Editável | Mesmo valor e persistência do desktop |
| GPU | `spec_mru_perm_probe` | Editável | Mesmo valor e persistência do desktop |
| GPU | `static_color_write_mask` | Editável | Mesmo valor e persistência do desktop |
| GPU | `stream_barrier_skip` | Editável | Mesmo valor e persistência do desktop |
| GPU | `stream_copy_idle_us` | Editável | Mesmo valor e persistência do desktop |
| GPU | `stream_copy_lane_threads` | Editável | Mesmo valor e persistência do desktop |
| GPU | `stream_copy_workers` | Editável | Mesmo valor e persistência do desktop |
| GPU | `texture_invalidate_filter` | Editável | Mesmo valor e persistência do desktop |
| GPU | `texture_lru_lazy_touch` | Editável | Mesmo valor e persistência do desktop |
| GPU | `texture_view_memo` | Editável | Mesmo valor e persistência do desktop |
| GPU | `tracker_lock_spin_rounds` | Editável | Mesmo valor e persistência do desktop |
| GPU | `userfaultfd` | Excluída | userfaultfd é uma API Linux, indisponível no UWP. |
| GPU | `vblank_frequency` | Editável | Mesmo valor e persistência do desktop |
| GPU | `vertex_layout_memo` | Editável | Mesmo valor e persistência do desktop |
| GPU | `window_height` | Editável | Mesmo valor e persistência do desktop |
| GPU | `window_width` | Editável | Mesmo valor e persistência do desktop |
| Vulkan | `gpu_id` | Editável | Mesmo valor e persistência do desktop |
| Vulkan | `pipeline_cache_archived` | Editável | Mesmo valor e persistência do desktop |
| Vulkan | `pipeline_cache_enabled` | Editável | Mesmo valor e persistência do desktop |
| Vulkan | `renderdoc_enabled` | Excluída | As ferramentas de validação/captura desktop não estão incluídas no pacote UWP. |
| Vulkan | `vkcrash_diagnostic_enabled` | Excluída | As ferramentas de validação/captura desktop não estão incluídas no pacote UWP. |
| Vulkan | `vkguest_markers` | Editável | Mesmo valor e persistência do desktop |
| Vulkan | `vkhost_markers` | Editável | Mesmo valor e persistência do desktop |
| Vulkan | `vkvalidation_core_enabled` | Excluída | As ferramentas de validação/captura desktop não estão incluídas no pacote UWP. |
| Vulkan | `vkvalidation_enabled` | Excluída | As ferramentas de validação/captura desktop não estão incluídas no pacote UWP. |
| Vulkan | `vkvalidation_gpu_enabled` | Excluída | As ferramentas de validação/captura desktop não estão incluídas no pacote UWP. |
| Vulkan | `vkvalidation_sync_enabled` | Excluída | As ferramentas de validação/captura desktop não estão incluídas no pacote UWP. |
