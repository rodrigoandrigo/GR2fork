# Configurações do frontend UWP

Inventário extraído pela API da DLL a partir dos oito grupos reais de EmulatorSettings: 167 opções, 46 editáveis nesta base UWP e 121 excluídas. As exclusões são limitações deste port atual, não uma afirmação de impossibilidade na plataforma. Valores excluídos são preservados.

Os perfis por jogo usam os mesmos campos overrideable e arquivos do desktop. Campos globais permanecem somente leitura ao selecionar um jogo; excluir o perfil restaura a herança global. Aplicações inválidas retornam erro e não alteram o valor salvo.

Pastas de biblioteca são selecionadas pelo broker, com grants na FutureAccessList. Importações para módulos, fontes e DLCs usam o armazenamento privado. Renderização PS4 D3D12, sensores, integração Discord e adaptadores desktop ausentes ainda exigem implementação.

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
| Debug | `shader_collect` | Excluída | A coleta de shaders usa o renderer Vulkan. |
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
| Audio | `audio_backend` | Editável | SDL3 (0), OpenAL (1) ou XAudio2 (2); novas configurações UWP usam XAudio2 |
| Audio | `openal_hrtf` | Editável | Mesmo valor e persistência do desktop |
| Audio | `openal_main_output_device` | Editável | Mesmo valor e persistência do desktop |
| Audio | `openal_mic_device` | Editável | Mesmo valor e persistência do desktop |
| Audio | `openal_output_mode` | Editável | Mesmo valor e persistência do desktop |
| Audio | `openal_padSpk_output_device` | Editável | Mesmo valor e persistência do desktop |
| Audio | `sdl_main_output_device` | Editável | Mesmo valor e persistência do desktop |
| Audio | `sdl_mic_device` | Editável | Mesmo valor e persistência do desktop |
| Audio | `sdl_padSpk_output_device` | Editável | Mesmo valor e persistência do desktop |
| GPU | `adaptive_skipcaches_mode` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `backing_write_memo` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `bind_image_lean` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `bind_line_prefetch` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `bind_noop_memo` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `bind_write_plan` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `br_mem_fast_state` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `clean_sync_peek` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `copy_gpu_buffers` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `covered_range_skip` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `desc_delta_flat` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `desc_delta_inplace` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `desc_delta_partial` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `desc_heap_recycle` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `desc_layout_share` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `direct_memory_access_enabled` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `draw_glue_memo` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `dump_shaders` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `dyn_state_memo` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `dyn_state_stamp` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `findimg_memo_entries` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `findimg_memo_first` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `findimg_memo_ways` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `findimg_range_invalidate` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `findimg_slot_hint` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `findimg_trust_gen` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `flush_draw_interval` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `fsr_enabled` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `full_screen` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `full_screen_mode` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `gather_input_memo` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `gpu_thread_core_reserve` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `guest_copy_hold_segment` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `guest_copy_lock_batch` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `hdr_allowed` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `image_fast_state` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `image_update_direct` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `inline_fetch_shader` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `internal_screen_height` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `internal_screen_width` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `key_reuse_hash_diff` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `null_gpu` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `occlude_all` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `one_thread_per_core` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `parser_reg_run` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `patch_shaders` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `pending_pop_throttle` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `pipeline_key_stamp_reuse` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `present_mode` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `push_const_dedup` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `push_desc_full_limit` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `push_vp_memo` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `rcas_attenuation` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `rcas_enabled` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `readback_linear_images_async` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `readback_linear_images_enabled` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `readback_offload` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `readbacks_mode` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `residency_bitmap` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `ri_memo_fused_cmp` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `ring_drain_flush_draws` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `rt_state_stamp` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `runtime_info_input_memo` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `runtime_info_stamp_gate` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `shader_params_memo` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `shader_params_memo_entries` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `spec_fp_cache` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `spec_fp_canonical` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `spec_fp_front` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `spec_fp_slot_inplace` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `spec_key_fast` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `spec_key_fused` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `spec_mru_perm_probe` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `static_color_write_mask` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `stream_barrier_skip` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `stream_copy_idle_us` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `stream_copy_lane_threads` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `stream_copy_workers` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `texture_invalidate_filter` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `texture_lru_lazy_touch` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `texture_view_memo` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `tracker_lock_spin_rounds` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `userfaultfd` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `vblank_frequency` | Editável | Mesmo valor e persistência do desktop |
| GPU | `vertex_layout_memo` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `window_height` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| GPU | `window_width` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| Vulkan | `gpu_id` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| Vulkan | `pipeline_cache_archived` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| Vulkan | `pipeline_cache_enabled` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| Vulkan | `renderdoc_enabled` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| Vulkan | `vkcrash_diagnostic_enabled` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| Vulkan | `vkguest_markers` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| Vulkan | `vkhost_markers` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| Vulkan | `vkvalidation_core_enabled` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| Vulkan | `vkvalidation_enabled` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| Vulkan | `vkvalidation_gpu_enabled` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |
| Vulkan | `vkvalidation_sync_enabled` | Excluída | Esta opção usa o renderer Vulkan desktop, ausente no caminho D3D12 UWP. |

