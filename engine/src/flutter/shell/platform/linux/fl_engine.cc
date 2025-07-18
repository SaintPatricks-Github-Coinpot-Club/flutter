// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/public/flutter_linux/fl_engine.h"

#include <epoxy/egl.h>
#include <gmodule.h>

#include <cstring>

#include "flutter/common/constants.h"
#include "flutter/shell/platform/common/engine_switches.h"
#include "flutter/shell/platform/embedder/embedder.h"
#include "flutter/shell/platform/linux/fl_binary_messenger_private.h"
#include "flutter/shell/platform/linux/fl_dart_project_private.h"
#include "flutter/shell/platform/linux/fl_display_monitor.h"
#include "flutter/shell/platform/linux/fl_engine_private.h"
#include "flutter/shell/platform/linux/fl_framebuffer.h"
#include "flutter/shell/platform/linux/fl_keyboard_handler.h"
#include "flutter/shell/platform/linux/fl_opengl_manager.h"
#include "flutter/shell/platform/linux/fl_pixel_buffer_texture_private.h"
#include "flutter/shell/platform/linux/fl_platform_handler.h"
#include "flutter/shell/platform/linux/fl_plugin_registrar_private.h"
#include "flutter/shell/platform/linux/fl_settings_handler.h"
#include "flutter/shell/platform/linux/fl_texture_gl_private.h"
#include "flutter/shell/platform/linux/fl_texture_registrar_private.h"
#include "flutter/shell/platform/linux/fl_windowing_handler.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_plugin_registry.h"

// Unique number associated with platform tasks.
static constexpr size_t kPlatformTaskRunnerIdentifier = 1;

// Use different device ID for mouse and pan/zoom events, since we can't
// differentiate the actual device (mouse v.s. trackpad)
static constexpr int32_t kMousePointerDeviceId = 0;
static constexpr int32_t kPointerPanZoomDeviceId = 1;

struct _FlEngine {
  GObject parent_instance;

  // Thread the GLib main loop is running on.
  GThread* thread;

  // The project this engine is running.
  FlDartProject* project;

  // Watches for monitors changes to update engine.
  FlDisplayMonitor* display_monitor;

  // Type of rendering performed.
  FlutterRendererType renderer_type;

  // Manages OpenGL contexts.
  FlOpenGLManager* opengl_manager;

  // Messenger used to send and receive platform messages.
  FlBinaryMessenger* binary_messenger;

  // Implements the flutter/settings channel.
  FlSettingsHandler* settings_handler;

  // Implements the flutter/platform channel.
  FlPlatformHandler* platform_handler;

  // Implements the flutter/windowing channel.
  FlWindowingHandler* windowing_handler;

  // Process keyboard events.
  FlKeyboardManager* keyboard_manager;

  // Implements the flutter/textinput channel.
  FlTextInputHandler* text_input_handler;

  // Implements the flutter/keyboard channel.
  FlKeyboardHandler* keyboard_handler;

  // Implements the flutter/mousecursor channel.
  FlMouseCursorHandler* mouse_cursor_handler;

  // Manages textures rendered by native code.
  FlTextureRegistrar* texture_registrar;

  // Schedules tasks to be run on the appropriate thread.
  FlTaskRunner* task_runner;

  // Ahead of time data used to make engine run faster.
  FlutterEngineAOTData aot_data;

  // The Flutter engine.
  FLUTTER_API_SYMBOL(FlutterEngine) engine;

  // Function table for engine API, used to intercept engine calls for testing
  // purposes.
  FlutterEngineProcTable embedder_api;

  // Next ID to use for a view.
  FlutterViewId next_view_id;

  // Objects rendering the views.
  GHashTable* renderables_by_view_id;

  // Function to call when a platform message is received.
  FlEnginePlatformMessageHandler platform_message_handler;
  gpointer platform_message_handler_data;
  GDestroyNotify platform_message_handler_destroy_notify;
};

G_DEFINE_QUARK(fl_engine_error_quark, fl_engine_error)

static void fl_engine_plugin_registry_iface_init(
    FlPluginRegistryInterface* iface);

enum { SIGNAL_ON_PRE_ENGINE_RESTART, SIGNAL_UPDATE_SEMANTICS, LAST_SIGNAL };

static guint fl_engine_signals[LAST_SIGNAL];

G_DEFINE_TYPE_WITH_CODE(
    FlEngine,
    fl_engine,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(fl_plugin_registry_get_type(),
                          fl_engine_plugin_registry_iface_init))

enum { PROP_0, PROP_BINARY_MESSENGER, PROP_LAST };

// Parse a locale into its components.
static void parse_locale(const gchar* locale,
                         gchar** language,
                         gchar** territory,
                         gchar** codeset,
                         gchar** modifier) {
  gchar* l = g_strdup(locale);

  // Locales are in the form "language[_territory][.codeset][@modifier]"
  gchar* match = strrchr(l, '@');
  if (match != nullptr) {
    if (modifier != nullptr) {
      *modifier = g_strdup(match + 1);
    }
    *match = '\0';
  } else if (modifier != nullptr) {
    *modifier = nullptr;
  }

  match = strrchr(l, '.');
  if (match != nullptr) {
    if (codeset != nullptr) {
      *codeset = g_strdup(match + 1);
    }
    *match = '\0';
  } else if (codeset != nullptr) {
    *codeset = nullptr;
  }

  match = strrchr(l, '_');
  if (match != nullptr) {
    if (territory != nullptr) {
      *territory = g_strdup(match + 1);
    }
    *match = '\0';
  } else if (territory != nullptr) {
    *territory = nullptr;
  }

  if (language != nullptr) {
    *language = l;
  }
}

static void view_added_cb(const FlutterAddViewResult* result) {
  g_autoptr(GTask) task = G_TASK(result->user_data);

  if (result->added) {
    g_task_return_boolean(task, TRUE);
  } else {
    g_task_return_new_error(task, fl_engine_error_quark(),
                            FL_ENGINE_ERROR_FAILED, "Failed to add view");
  }
}

static void view_removed_cb(const FlutterRemoveViewResult* result) {
  g_autoptr(GTask) task = G_TASK(result->user_data);

  if (result->removed) {
    g_task_return_boolean(task, TRUE);
  } else {
    g_task_return_new_error(task, fl_engine_error_quark(),
                            FL_ENGINE_ERROR_FAILED, "Failed to remove view");
  }
}

static void free_locale(FlutterLocale* locale) {
  free(const_cast<gchar*>(locale->language_code));
  free(const_cast<gchar*>(locale->country_code));
  free(locale);
}

// Passes locale information to the Flutter engine.
static void setup_locales(FlEngine* self) {
  const gchar* const* languages = g_get_language_names();
  g_autoptr(GPtrArray) locales_array = g_ptr_array_new_with_free_func(
      reinterpret_cast<GDestroyNotify>(free_locale));
  for (int i = 0; languages[i] != nullptr; i++) {
    g_autofree gchar* locale_string = g_strstrip(g_strdup(languages[i]));

    // Ignore empty locales, caused by settings like `LANGUAGE=pt_BR:`
    if (strcmp(locale_string, "") == 0) {
      continue;
    }

    g_autofree gchar* language = nullptr;
    g_autofree gchar* territory = nullptr;
    parse_locale(locale_string, &language, &territory, nullptr, nullptr);

    // Ignore duplicate locales, caused by settings like `LANGUAGE=C` (returns
    // two "C") or `LANGUAGE=en:en`
    gboolean has_locale = FALSE;
    for (guint j = 0; !has_locale && j < locales_array->len; j++) {
      FlutterLocale* locale =
          reinterpret_cast<FlutterLocale*>(g_ptr_array_index(locales_array, j));
      has_locale = g_strcmp0(locale->language_code, language) == 0 &&
                   g_strcmp0(locale->country_code, territory) == 0;
    }
    if (has_locale) {
      continue;
    }

    FlutterLocale* locale =
        static_cast<FlutterLocale*>(g_malloc0(sizeof(FlutterLocale)));
    g_ptr_array_add(locales_array, locale);
    locale->struct_size = sizeof(FlutterLocale);
    locale->language_code =
        reinterpret_cast<const gchar*>(g_steal_pointer(&language));
    locale->country_code =
        reinterpret_cast<const gchar*>(g_steal_pointer(&territory));
    locale->script_code = nullptr;
    locale->variant_code = nullptr;
  }
  FlutterLocale** locales =
      reinterpret_cast<FlutterLocale**>(locales_array->pdata);
  FlutterEngineResult result = self->embedder_api.UpdateLocales(
      self->engine, const_cast<const FlutterLocale**>(locales),
      locales_array->len);
  if (result != kSuccess) {
    g_warning("Failed to set up Flutter locales");
  }
}

static bool create_opengl_backing_store(
    FlEngine* self,
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* backing_store_out) {
  fl_opengl_manager_make_current(self->opengl_manager);

  GLint sized_format = GL_RGBA8;
  GLint general_format = GL_RGBA;
  if (epoxy_has_gl_extension("GL_EXT_texture_format_BGRA8888")) {
    sized_format = GL_BGRA8_EXT;
    general_format = GL_BGRA_EXT;
  }

  FlFramebuffer* framebuffer = fl_framebuffer_new(
      general_format, config->size.width, config->size.height, FALSE);
  if (!framebuffer) {
    g_warning("Failed to create backing store");
    return false;
  }

  backing_store_out->type = kFlutterBackingStoreTypeOpenGL;
  backing_store_out->open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
  backing_store_out->open_gl.framebuffer.user_data = framebuffer;
  backing_store_out->open_gl.framebuffer.name =
      fl_framebuffer_get_id(framebuffer);
  backing_store_out->open_gl.framebuffer.target = sized_format;
  backing_store_out->open_gl.framebuffer.destruction_callback = [](void* p) {
    // Backing store destroyed in fl_compositor_opengl_collect_backing_store(),
    // set on FlutterCompositor.collect_backing_store_callback during engine
    // start.
  };

  return true;
}

static bool collect_opengl_backing_store(
    FlEngine* self,
    const FlutterBackingStore* backing_store) {
  fl_opengl_manager_make_current(self->opengl_manager);

  // OpenGL context is required when destroying #FlFramebuffer.
  g_object_unref(backing_store->open_gl.framebuffer.user_data);
  return true;
}

static bool create_software_backing_store(
    FlEngine* self,
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* backing_store_out) {
  size_t allocation_length = config->size.width * config->size.height * 4;
  uint8_t* allocation = static_cast<uint8_t*>(malloc(allocation_length));
  if (allocation == nullptr) {
    return false;
  }

  backing_store_out->type = kFlutterBackingStoreTypeSoftware;
  backing_store_out->software.allocation = allocation;
  backing_store_out->software.height = config->size.height;
  backing_store_out->software.row_bytes = config->size.width * 4;
  backing_store_out->software.user_data = nullptr;
  backing_store_out->software.destruction_callback = [](void* p) {
    // Backing store destroyed in
    // fl_compositor_software_collect_backing_store(), set on
    // FlutterCompositor.collect_backing_store_callback during engine start.
  };

  return true;
}

static bool collect_software_backing_store(
    FlEngine* self,
    const FlutterBackingStore* backing_store) {
  free(const_cast<void*>(backing_store->software.allocation));
  return true;
}

// Called when engine needs a backing store for a specific #FlutterLayer.
static bool compositor_create_backing_store_callback(
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* backing_store_out,
    void* user_data) {
  FlEngine* self = static_cast<FlEngine*>(user_data);
  switch (self->renderer_type) {
    case kOpenGL:
      return create_opengl_backing_store(self, config, backing_store_out);
    case kSoftware:
      return create_software_backing_store(self, config, backing_store_out);
    default:
      return false;
  }
}

// Called when the backing store is to be released.
static bool compositor_collect_backing_store_callback(
    const FlutterBackingStore* backing_store,
    void* user_data) {
  FlEngine* self = static_cast<FlEngine*>(user_data);
  switch (self->renderer_type) {
    case kOpenGL:
      return collect_opengl_backing_store(self, backing_store);
    case kSoftware:
      return collect_software_backing_store(self, backing_store);
    default:
      return false;
  }
}

// Called when embedder should composite contents of each layer onto the screen.
static bool compositor_present_view_callback(
    const FlutterPresentViewInfo* info) {
  FlEngine* self = static_cast<FlEngine*>(info->user_data);

  GWeakRef* ref = static_cast<GWeakRef*>(g_hash_table_lookup(
      self->renderables_by_view_id, GINT_TO_POINTER(info->view_id)));
  if (ref == nullptr) {
    return true;
  }
  g_autoptr(FlRenderable) renderable = FL_RENDERABLE(g_weak_ref_get(ref));
  if (renderable == nullptr) {
    return true;
  }

  fl_renderable_present_layers(renderable, info->layers, info->layers_count);
  return true;
}

// Flutter engine rendering callbacks.

static void* fl_engine_gl_proc_resolver(void* user_data, const char* name) {
  return reinterpret_cast<void*>(eglGetProcAddress(name));
}

static bool fl_engine_gl_make_current(void* user_data) {
  FlEngine* self = static_cast<FlEngine*>(user_data);
  fl_opengl_manager_make_current(self->opengl_manager);
  return true;
}

static bool fl_engine_gl_clear_current(void* user_data) {
  FlEngine* self = static_cast<FlEngine*>(user_data);
  fl_opengl_manager_clear_current(self->opengl_manager);
  return true;
}

static uint32_t fl_engine_gl_get_fbo(void* user_data) {
  // There is only one frame buffer object - always return that.
  return 0;
}

static bool fl_engine_gl_make_resource_current(void* user_data) {
  FlEngine* self = static_cast<FlEngine*>(user_data);
  fl_opengl_manager_make_resource_current(self->opengl_manager);
  return true;
}

// Called by the engine to retrieve an external texture.
static bool fl_engine_gl_external_texture_frame_callback(
    void* user_data,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterOpenGLTexture* opengl_texture) {
  FlEngine* self = static_cast<FlEngine*>(user_data);
  if (!self->texture_registrar) {
    return false;
  }

  FlTexture* texture =
      fl_texture_registrar_lookup_texture(self->texture_registrar, texture_id);
  if (texture == nullptr) {
    g_warning("Unable to find texture %" G_GINT64_FORMAT, texture_id);
    return false;
  }

  gboolean result;
  g_autoptr(GError) error = nullptr;
  if (FL_IS_TEXTURE_GL(texture)) {
    result = fl_texture_gl_populate(FL_TEXTURE_GL(texture), width, height,
                                    opengl_texture, &error);
  } else if (FL_IS_PIXEL_BUFFER_TEXTURE(texture)) {
    result =
        fl_pixel_buffer_texture_populate(FL_PIXEL_BUFFER_TEXTURE(texture),
                                         width, height, opengl_texture, &error);
  } else {
    g_warning("Unsupported texture type %" G_GINT64_FORMAT, texture_id);
    return false;
  }

  if (!result) {
    g_warning("%s", error->message);
    return false;
  }

  return true;
}

// Called by the engine to determine if it is on the GTK thread.
static bool fl_engine_runs_task_on_current_thread(void* user_data) {
  FlEngine* self = static_cast<FlEngine*>(user_data);
  return self->thread == g_thread_self();
}

// Called when the engine has a task to perform in the GTK thread.
static void fl_engine_post_task(FlutterTask task,
                                uint64_t target_time_nanos,
                                void* user_data) {
  FlEngine* self = static_cast<FlEngine*>(user_data);

  fl_task_runner_post_flutter_task(self->task_runner, task, target_time_nanos);
}

// Called when a platform message is received from the engine.
static void fl_engine_platform_message_cb(const FlutterPlatformMessage* message,
                                          void* user_data) {
  FlEngine* self = FL_ENGINE(user_data);

  gboolean handled = FALSE;
  if (self->platform_message_handler != nullptr) {
    g_autoptr(GBytes) data =
        g_bytes_new(message->message, message->message_size);
    handled = self->platform_message_handler(
        self, message->channel, data, message->response_handle,
        self->platform_message_handler_data);
  }

  if (!handled) {
    fl_engine_send_platform_message_response(self, message->response_handle,
                                             nullptr, nullptr);
  }
}

// Called when a semantic node update is received from the engine.
static void fl_engine_update_semantics_cb(const FlutterSemanticsUpdate2* update,
                                          void* user_data) {
  FlEngine* self = FL_ENGINE(user_data);

  g_signal_emit(self, fl_engine_signals[SIGNAL_UPDATE_SEMANTICS], 0, update);
}

static void setup_keyboard(FlEngine* self) {
  g_clear_object(&self->keyboard_manager);
  self->keyboard_manager = fl_keyboard_manager_new(self);

  g_clear_object(&self->keyboard_handler);
  self->keyboard_handler =
      fl_keyboard_handler_new(self->binary_messenger, self->keyboard_manager);

  GtkWidget* widget =
      self->text_input_handler != nullptr
          ? fl_text_input_handler_get_widget(self->text_input_handler)
          : nullptr;
  g_clear_object(&self->text_input_handler);
  self->text_input_handler = fl_text_input_handler_new(self->binary_messenger);
  if (widget != nullptr) {
    fl_text_input_handler_set_widget(self->text_input_handler, widget);
  }
}

// Called right before the engine is restarted.
//
// This method should reset states to as if the engine has just been started,
// which usually indicates the user has requested a hot restart (Shift-R in the
// Flutter CLI.)
static void fl_engine_on_pre_engine_restart_cb(void* user_data) {
  FlEngine* self = FL_ENGINE(user_data);

  setup_keyboard(self);

  g_signal_emit(self, fl_engine_signals[SIGNAL_ON_PRE_ENGINE_RESTART], 0);
}

// Called when a response to a sent platform message is received from the
// engine.
static void fl_engine_platform_message_response_cb(const uint8_t* data,
                                                   size_t data_length,
                                                   void* user_data) {
  g_autoptr(GTask) task = G_TASK(user_data);
  g_task_return_pointer(task, g_bytes_new(data, data_length),
                        reinterpret_cast<GDestroyNotify>(g_bytes_unref));
}

// Implements FlPluginRegistry::get_registrar_for_plugin.
static FlPluginRegistrar* fl_engine_get_registrar_for_plugin(
    FlPluginRegistry* registry,
    const gchar* name) {
  FlEngine* self = FL_ENGINE(registry);

  return fl_plugin_registrar_new(nullptr, self->binary_messenger,
                                 self->texture_registrar);
}

static void fl_engine_plugin_registry_iface_init(
    FlPluginRegistryInterface* iface) {
  iface->get_registrar_for_plugin = fl_engine_get_registrar_for_plugin;
}

static void fl_engine_set_property(GObject* object,
                                   guint prop_id,
                                   const GValue* value,
                                   GParamSpec* pspec) {
  FlEngine* self = FL_ENGINE(object);
  switch (prop_id) {
    case PROP_BINARY_MESSENGER:
      g_set_object(&self->binary_messenger,
                   FL_BINARY_MESSENGER(g_value_get_object(value)));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void fl_engine_dispose(GObject* object) {
  FlEngine* self = FL_ENGINE(object);

  if (self->engine != nullptr) {
    self->embedder_api.Shutdown(self->engine);
    self->engine = nullptr;
  }

  if (self->aot_data != nullptr) {
    self->embedder_api.CollectAOTData(self->aot_data);
    self->aot_data = nullptr;
  }

  fl_binary_messenger_shutdown(self->binary_messenger);
  fl_texture_registrar_shutdown(self->texture_registrar);

  g_clear_object(&self->project);
  g_clear_object(&self->display_monitor);
  g_clear_object(&self->opengl_manager);
  g_clear_object(&self->texture_registrar);
  g_clear_object(&self->binary_messenger);
  g_clear_object(&self->settings_handler);
  g_clear_object(&self->platform_handler);
  g_clear_object(&self->windowing_handler);
  g_clear_object(&self->keyboard_manager);
  g_clear_object(&self->text_input_handler);
  g_clear_object(&self->keyboard_handler);
  g_clear_object(&self->mouse_cursor_handler);
  g_clear_object(&self->task_runner);
  g_clear_pointer(&self->renderables_by_view_id, g_hash_table_unref);

  if (self->platform_message_handler_destroy_notify) {
    self->platform_message_handler_destroy_notify(
        self->platform_message_handler_data);
  }
  self->platform_message_handler_data = nullptr;
  self->platform_message_handler_destroy_notify = nullptr;

  G_OBJECT_CLASS(fl_engine_parent_class)->dispose(object);
}

static void fl_engine_class_init(FlEngineClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_engine_dispose;
  G_OBJECT_CLASS(klass)->set_property = fl_engine_set_property;

  g_object_class_install_property(
      G_OBJECT_CLASS(klass), PROP_BINARY_MESSENGER,
      g_param_spec_object(
          "binary-messenger", "messenger", "Binary messenger",
          fl_binary_messenger_get_type(),
          static_cast<GParamFlags>(G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY |
                                   G_PARAM_STATIC_STRINGS)));

  fl_engine_signals[SIGNAL_ON_PRE_ENGINE_RESTART] = g_signal_new(
      "on-pre-engine-restart", fl_engine_get_type(), G_SIGNAL_RUN_LAST, 0,
      nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  fl_engine_signals[SIGNAL_UPDATE_SEMANTICS] = g_signal_new(
      "update-semantics", fl_engine_get_type(), G_SIGNAL_RUN_LAST, 0, nullptr,
      nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void fl_engine_init(FlEngine* self) {
  self->thread = g_thread_self();

  self->embedder_api.struct_size = sizeof(FlutterEngineProcTable);
  if (FlutterEngineGetProcAddresses(&self->embedder_api) != kSuccess) {
    g_warning("Failed get get engine function pointers");
  }

  self->opengl_manager = fl_opengl_manager_new();

  self->display_monitor =
      fl_display_monitor_new(self, gdk_display_get_default());
  self->task_runner = fl_task_runner_new(self);

  // Implicit view is 0, so start at 1.
  self->next_view_id = 1;
  self->renderables_by_view_id = g_hash_table_new_full(
      g_direct_hash, g_direct_equal, nullptr, [](gpointer value) {
        GWeakRef* ref = static_cast<GWeakRef*>(value);
        g_weak_ref_clear(ref);
        free(ref);
      });

  self->texture_registrar = fl_texture_registrar_new(self);
}

static FlEngine* fl_engine_new_full(FlDartProject* project,
                                    FlBinaryMessenger* binary_messenger) {
  g_return_val_if_fail(FL_IS_DART_PROJECT(project), nullptr);

  FlEngine* self = FL_ENGINE(g_object_new(fl_engine_get_type(), nullptr));

  self->project = FL_DART_PROJECT(g_object_ref(project));
  const gchar* renderer = g_getenv("FLUTTER_LINUX_RENDERER");
  if (g_strcmp0(renderer, "software") == 0) {
    self->renderer_type = kSoftware;
    g_warning(
        "Using the software renderer. Not all features are supported. This is "
        "not recommended.\n"
        "\n"
        "To switch back to the default renderer, unset the "
        "FLUTTER_LINUX_RENDERER environment variable.");
  } else {
    if (renderer != nullptr && strcmp(renderer, "opengl") != 0) {
      g_warning("Unknown renderer type '%s', defaulting to opengl", renderer);
    }
    self->renderer_type = kOpenGL;
  }

  if (binary_messenger != nullptr) {
    self->binary_messenger =
        FL_BINARY_MESSENGER(g_object_ref(binary_messenger));
  } else {
    self->binary_messenger = fl_binary_messenger_new(self);
  }
  self->keyboard_manager = fl_keyboard_manager_new(self);
  self->mouse_cursor_handler =
      fl_mouse_cursor_handler_new(self->binary_messenger);
  self->windowing_handler = fl_windowing_handler_new(self);

  return self;
}

FlEngine* fl_engine_for_id(int64_t id) {
  void* engine = reinterpret_cast<void*>(id);
  g_return_val_if_fail(FL_IS_ENGINE(engine), nullptr);
  return FL_ENGINE(engine);
}

G_MODULE_EXPORT FlEngine* fl_engine_new(FlDartProject* project) {
  return fl_engine_new_full(project, nullptr);
}

FlEngine* fl_engine_new_with_binary_messenger(
    FlBinaryMessenger* binary_messenger) {
  g_autoptr(FlDartProject) project = fl_dart_project_new();
  return fl_engine_new_full(project, binary_messenger);
}

G_MODULE_EXPORT FlEngine* fl_engine_new_headless(FlDartProject* project) {
  return fl_engine_new(project);
}

FlutterRendererType fl_engine_get_renderer_type(FlEngine* self) {
  g_return_val_if_fail(FL_IS_ENGINE(self), static_cast<FlutterRendererType>(0));
  return self->renderer_type;
}

FlOpenGLManager* fl_engine_get_opengl_manager(FlEngine* self) {
  g_return_val_if_fail(FL_IS_ENGINE(self), nullptr);
  return self->opengl_manager;
}

FlDisplayMonitor* fl_engine_get_display_monitor(FlEngine* self) {
  g_return_val_if_fail(FL_IS_ENGINE(self), nullptr);
  return self->display_monitor;
}

gboolean fl_engine_start(FlEngine* self, GError** error) {
  g_return_val_if_fail(FL_IS_ENGINE(self), FALSE);

  FlutterRendererConfig config = {};
  config.type = self->renderer_type;
  switch (config.type) {
    case kSoftware:
      config.software.struct_size = sizeof(FlutterSoftwareRendererConfig);
      // No action required, as this is handled in
      // compositor_present_view_callback.
      config.software.surface_present_callback =
          [](void* user_data, const void* allocation, size_t row_bytes,
             size_t height) { return true; };
      break;
    case kOpenGL:
      config.open_gl.struct_size = sizeof(FlutterOpenGLRendererConfig);
      config.open_gl.gl_proc_resolver = fl_engine_gl_proc_resolver;
      config.open_gl.make_current = fl_engine_gl_make_current;
      config.open_gl.clear_current = fl_engine_gl_clear_current;
      config.open_gl.fbo_callback = fl_engine_gl_get_fbo;
      // No action required, as this is handled in
      // compositor_present_view_callback.
      config.open_gl.present = [](void* user_data) { return true; };
      config.open_gl.make_resource_current = fl_engine_gl_make_resource_current;
      config.open_gl.gl_external_texture_frame_callback =
          fl_engine_gl_external_texture_frame_callback;
      break;
    case kMetal:
    case kVulkan:
    default:
      g_set_error(error, fl_engine_error_quark(), FL_ENGINE_ERROR_FAILED,
                  "Unsupported renderer type");
      return FALSE;
  }

  FlutterTaskRunnerDescription platform_task_runner = {};
  platform_task_runner.struct_size = sizeof(FlutterTaskRunnerDescription);
  platform_task_runner.user_data = self;
  platform_task_runner.runs_task_on_current_thread_callback =
      fl_engine_runs_task_on_current_thread;
  platform_task_runner.post_task_callback = fl_engine_post_task;
  platform_task_runner.identifier = kPlatformTaskRunnerIdentifier;

  FlutterCustomTaskRunners custom_task_runners = {};
  custom_task_runners.struct_size = sizeof(FlutterCustomTaskRunners);
  custom_task_runners.platform_task_runner = &platform_task_runner;

  if (fl_dart_project_get_ui_thread_policy(self->project) ==
      FL_UI_THREAD_POLICY_RUN_ON_PLATFORM_THREAD) {
    custom_task_runners.ui_task_runner = &platform_task_runner;
  }

  g_autoptr(GPtrArray) command_line_args =
      g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_insert(command_line_args, 0, g_strdup("flutter"));
  for (const auto& env_switch : flutter::GetSwitchesFromEnvironment()) {
    g_ptr_array_add(command_line_args, g_strdup(env_switch.c_str()));
  }

  gchar** dart_entrypoint_args =
      fl_dart_project_get_dart_entrypoint_arguments(self->project);

  FlutterProjectArgs args = {};
  args.struct_size = sizeof(FlutterProjectArgs);
  args.assets_path = fl_dart_project_get_assets_path(self->project);
  args.icu_data_path = fl_dart_project_get_icu_data_path(self->project);
  args.command_line_argc = command_line_args->len;
  args.command_line_argv =
      reinterpret_cast<const char* const*>(command_line_args->pdata);
  args.platform_message_callback = fl_engine_platform_message_cb;
  args.update_semantics_callback2 = fl_engine_update_semantics_cb;
  args.custom_task_runners = &custom_task_runners;
  args.shutdown_dart_vm_when_done = true;
  args.on_pre_engine_restart_callback = fl_engine_on_pre_engine_restart_cb;
  args.dart_entrypoint_argc =
      dart_entrypoint_args != nullptr ? g_strv_length(dart_entrypoint_args) : 0;
  args.dart_entrypoint_argv =
      reinterpret_cast<const char* const*>(dart_entrypoint_args);
  args.engine_id = reinterpret_cast<int64_t>(self);

  FlutterCompositor compositor = {};
  compositor.struct_size = sizeof(FlutterCompositor);
  compositor.user_data = self;
  compositor.create_backing_store_callback =
      compositor_create_backing_store_callback;
  compositor.collect_backing_store_callback =
      compositor_collect_backing_store_callback;
  compositor.present_view_callback = compositor_present_view_callback;
  args.compositor = &compositor;

  if (self->embedder_api.RunsAOTCompiledDartCode()) {
    FlutterEngineAOTDataSource source = {};
    source.type = kFlutterEngineAOTDataSourceTypeElfPath;
    source.elf_path = fl_dart_project_get_aot_library_path(self->project);
    if (self->embedder_api.CreateAOTData(&source, &self->aot_data) !=
        kSuccess) {
      g_set_error(error, fl_engine_error_quark(), FL_ENGINE_ERROR_FAILED,
                  "Failed to create AOT data");
      return FALSE;
    }
    args.aot_data = self->aot_data;
  }

  FlutterEngineResult result = self->embedder_api.Initialize(
      FLUTTER_ENGINE_VERSION, &config, &args, self, &self->engine);
  if (result != kSuccess) {
    g_set_error(error, fl_engine_error_quark(), FL_ENGINE_ERROR_FAILED,
                "Failed to initialize Flutter engine");
    return FALSE;
  }

  result = self->embedder_api.RunInitialized(self->engine);
  if (result != kSuccess) {
    g_set_error(error, fl_engine_error_quark(), FL_ENGINE_ERROR_FAILED,
                "Failed to run Flutter engine");
    return FALSE;
  }

  setup_locales(self);

  g_autoptr(FlSettings) settings = fl_settings_new();
  self->settings_handler = fl_settings_handler_new(self);
  fl_settings_handler_start(self->settings_handler, settings);

  self->platform_handler = fl_platform_handler_new(self->binary_messenger);

  setup_keyboard(self);

  result = self->embedder_api.UpdateSemanticsEnabled(self->engine, TRUE);
  if (result != kSuccess) {
    g_warning("Failed to enable accessibility features on Flutter engine");
  }

  fl_display_monitor_start(self->display_monitor);

  return TRUE;
}

FlutterEngineProcTable* fl_engine_get_embedder_api(FlEngine* self) {
  return &(self->embedder_api);
}

void fl_engine_notify_display_update(FlEngine* self,
                                     const FlutterEngineDisplay* displays,
                                     size_t displays_length) {
  g_return_if_fail(FL_IS_ENGINE(self));

  FlutterEngineResult result = self->embedder_api.NotifyDisplayUpdate(
      self->engine, kFlutterEngineDisplaysUpdateTypeStartup, displays,
      displays_length);
  if (result != kSuccess) {
    g_warning("Failed to notify display update to Flutter engine: %d", result);
  }
}

void fl_engine_set_implicit_view(FlEngine* self, FlRenderable* renderable) {
  GWeakRef* ref = g_new(GWeakRef, 1);
  g_weak_ref_init(ref, G_OBJECT(renderable));
  g_hash_table_insert(self->renderables_by_view_id,
                      GINT_TO_POINTER(flutter::kFlutterImplicitViewId), ref);
}

FlutterViewId fl_engine_add_view(FlEngine* self,
                                 FlRenderable* renderable,
                                 size_t width,
                                 size_t height,
                                 double pixel_ratio,
                                 GCancellable* cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data) {
  g_return_val_if_fail(FL_IS_ENGINE(self), -1);

  g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);

  FlutterViewId view_id = self->next_view_id;
  self->next_view_id++;

  GWeakRef* ref = g_new(GWeakRef, 1);
  g_weak_ref_init(ref, G_OBJECT(renderable));
  g_hash_table_insert(self->renderables_by_view_id, GINT_TO_POINTER(view_id),
                      ref);

  // We don't know which display this view will open on, so set to zero and this
  // will be updated in a following FlutterWindowMetricsEvent
  FlutterEngineDisplayId display_id = 0;

  FlutterWindowMetricsEvent metrics = {};
  metrics.struct_size = sizeof(FlutterWindowMetricsEvent);
  metrics.width = width;
  metrics.height = height;
  metrics.pixel_ratio = pixel_ratio;
  metrics.display_id = display_id;
  metrics.view_id = view_id;
  FlutterAddViewInfo info;
  info.struct_size = sizeof(FlutterAddViewInfo);
  info.view_id = view_id;
  info.view_metrics = &metrics;
  info.user_data = g_object_ref(task);
  info.add_view_callback = view_added_cb;
  FlutterEngineResult result = self->embedder_api.AddView(self->engine, &info);
  if (result != kSuccess) {
    g_task_return_new_error(task, fl_engine_error_quark(),
                            FL_ENGINE_ERROR_FAILED, "AddView returned %d",
                            result);
    // This would have been done in the callback, but that won't occur now.
    g_object_unref(task);
  }

  return view_id;
}

gboolean fl_engine_add_view_finish(FlEngine* self,
                                   GAsyncResult* result,
                                   GError** error) {
  g_return_val_if_fail(FL_IS_ENGINE(self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

FlRenderable* fl_engine_get_renderable(FlEngine* self, FlutterViewId view_id) {
  g_return_val_if_fail(FL_IS_ENGINE(self), nullptr);

  GWeakRef* ref = static_cast<GWeakRef*>(g_hash_table_lookup(
      self->renderables_by_view_id, GINT_TO_POINTER(view_id)));
  return FL_RENDERABLE(g_weak_ref_get(ref));
}

void fl_engine_remove_view(FlEngine* self,
                           FlutterViewId view_id,
                           GCancellable* cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data) {
  g_return_if_fail(FL_IS_ENGINE(self));

  g_hash_table_remove(self->renderables_by_view_id, GINT_TO_POINTER(view_id));

  g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);

  FlutterRemoveViewInfo info;
  info.struct_size = sizeof(FlutterRemoveViewInfo);
  info.view_id = view_id;
  info.user_data = g_object_ref(task);
  info.remove_view_callback = view_removed_cb;
  FlutterEngineResult result =
      self->embedder_api.RemoveView(self->engine, &info);
  if (result != kSuccess) {
    g_task_return_new_error(task, fl_engine_error_quark(),
                            FL_ENGINE_ERROR_FAILED, "RemoveView returned %d",
                            result);
    // This would have been done in the callback, but that won't occur now.
    g_object_unref(task);
  }
}

gboolean fl_engine_remove_view_finish(FlEngine* self,
                                      GAsyncResult* result,
                                      GError** error) {
  g_return_val_if_fail(FL_IS_ENGINE(self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void fl_engine_set_platform_message_handler(
    FlEngine* self,
    FlEnginePlatformMessageHandler handler,
    gpointer user_data,
    GDestroyNotify destroy_notify) {
  g_return_if_fail(FL_IS_ENGINE(self));
  g_return_if_fail(handler != nullptr);

  if (self->platform_message_handler_destroy_notify) {
    self->platform_message_handler_destroy_notify(
        self->platform_message_handler_data);
  }

  self->platform_message_handler = handler;
  self->platform_message_handler_data = user_data;
  self->platform_message_handler_destroy_notify = destroy_notify;
}

// Note: This function can be called from any thread.
gboolean fl_engine_send_platform_message_response(
    FlEngine* self,
    const FlutterPlatformMessageResponseHandle* handle,
    GBytes* response,
    GError** error) {
  g_return_val_if_fail(FL_IS_ENGINE(self), FALSE);
  g_return_val_if_fail(handle != nullptr, FALSE);

  if (self->engine == nullptr) {
    g_set_error(error, fl_engine_error_quark(), FL_ENGINE_ERROR_FAILED,
                "No engine to send response to");
    return FALSE;
  }

  gsize data_length = 0;
  const uint8_t* data = nullptr;
  if (response != nullptr) {
    data =
        static_cast<const uint8_t*>(g_bytes_get_data(response, &data_length));
  }
  FlutterEngineResult result = self->embedder_api.SendPlatformMessageResponse(
      self->engine, handle, data, data_length);

  if (result != kSuccess) {
    g_set_error(error, fl_engine_error_quark(), FL_ENGINE_ERROR_FAILED,
                "Failed to send platform message response");
    return FALSE;
  }

  return TRUE;
}

void fl_engine_send_platform_message(FlEngine* self,
                                     const gchar* channel,
                                     GBytes* message,
                                     GCancellable* cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data) {
  g_return_if_fail(FL_IS_ENGINE(self));

  GTask* task = nullptr;
  FlutterPlatformMessageResponseHandle* response_handle = nullptr;
  if (callback != nullptr) {
    task = g_task_new(self, cancellable, callback, user_data);

    if (self->engine == nullptr) {
      g_task_return_new_error(task, fl_engine_error_quark(),
                              FL_ENGINE_ERROR_FAILED, "No engine to send to");
      return;
    }

    FlutterEngineResult result =
        self->embedder_api.PlatformMessageCreateResponseHandle(
            self->engine, fl_engine_platform_message_response_cb, task,
            &response_handle);
    if (result != kSuccess) {
      g_task_return_new_error(task, fl_engine_error_quark(),
                              FL_ENGINE_ERROR_FAILED,
                              "Failed to create response handle");
      g_object_unref(task);
      return;
    }
  } else if (self->engine == nullptr) {
    return;
  }

  FlutterPlatformMessage fl_message = {};
  fl_message.struct_size = sizeof(fl_message);
  fl_message.channel = channel;
  fl_message.message =
      message != nullptr
          ? static_cast<const uint8_t*>(g_bytes_get_data(message, nullptr))
          : nullptr;
  fl_message.message_size = message != nullptr ? g_bytes_get_size(message) : 0;
  fl_message.response_handle = response_handle;
  FlutterEngineResult result =
      self->embedder_api.SendPlatformMessage(self->engine, &fl_message);

  if (result != kSuccess && task != nullptr) {
    g_task_return_new_error(task, fl_engine_error_quark(),
                            FL_ENGINE_ERROR_FAILED,
                            "Failed to send platform messages");
    g_object_unref(task);
  }

  if (response_handle != nullptr) {
    self->embedder_api.PlatformMessageReleaseResponseHandle(self->engine,
                                                            response_handle);
  }
}

GBytes* fl_engine_send_platform_message_finish(FlEngine* self,
                                               GAsyncResult* result,
                                               GError** error) {
  g_return_val_if_fail(FL_IS_ENGINE(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  return static_cast<GBytes*>(g_task_propagate_pointer(G_TASK(result), error));
}

void fl_engine_send_window_metrics_event(FlEngine* self,
                                         FlutterEngineDisplayId display_id,
                                         FlutterViewId view_id,
                                         size_t width,
                                         size_t height,
                                         double pixel_ratio) {
  g_return_if_fail(FL_IS_ENGINE(self));

  if (self->engine == nullptr) {
    return;
  }

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(FlutterWindowMetricsEvent);
  event.width = width;
  event.height = height;
  event.pixel_ratio = pixel_ratio;
  event.display_id = display_id;
  event.view_id = view_id;
  self->embedder_api.SendWindowMetricsEvent(self->engine, &event);
}

void fl_engine_send_mouse_pointer_event(FlEngine* self,
                                        FlutterViewId view_id,
                                        FlutterPointerPhase phase,
                                        size_t timestamp,
                                        double x,
                                        double y,
                                        FlutterPointerDeviceKind device_kind,
                                        double scroll_delta_x,
                                        double scroll_delta_y,
                                        int64_t buttons) {
  g_return_if_fail(FL_IS_ENGINE(self));

  if (self->engine == nullptr) {
    return;
  }

  FlutterPointerEvent fl_event = {};
  fl_event.struct_size = sizeof(fl_event);
  fl_event.phase = phase;
  fl_event.timestamp = timestamp;
  fl_event.x = x;
  fl_event.y = y;
  if (scroll_delta_x != 0 || scroll_delta_y != 0) {
    fl_event.signal_kind = kFlutterPointerSignalKindScroll;
  }
  fl_event.scroll_delta_x = scroll_delta_x;
  fl_event.scroll_delta_y = scroll_delta_y;
  fl_event.device_kind = device_kind;
  fl_event.buttons = buttons;
  fl_event.device = kMousePointerDeviceId;
  fl_event.view_id = view_id;
  self->embedder_api.SendPointerEvent(self->engine, &fl_event, 1);
}

void fl_engine_send_touch_up_event(FlEngine* self,
                                   FlutterViewId view_id,
                                   size_t timestamp,
                                   double x,
                                   double y,
                                   int32_t device) {
  g_return_if_fail(FL_IS_ENGINE(self));

  if (self->engine == nullptr) {
    return;
  }

  FlutterPointerEvent event;
  event.timestamp = timestamp;
  event.x = x;
  event.y = y;
  event.device_kind = kFlutterPointerDeviceKindTouch;
  event.device = device;
  event.buttons = 0;
  event.view_id = view_id;
  event.phase = FlutterPointerPhase::kUp;
  event.struct_size = sizeof(event);

  self->embedder_api.SendPointerEvent(self->engine, &event, 1);
}

void fl_engine_send_touch_down_event(FlEngine* self,
                                     FlutterViewId view_id,
                                     size_t timestamp,
                                     double x,
                                     double y,
                                     int32_t device) {
  g_return_if_fail(FL_IS_ENGINE(self));

  if (self->engine == nullptr) {
    return;
  }

  FlutterPointerEvent event;
  event.timestamp = timestamp;
  event.x = x;
  event.y = y;
  event.device_kind = kFlutterPointerDeviceKindTouch;
  event.device = device;
  event.buttons = FlutterPointerMouseButtons::kFlutterPointerButtonMousePrimary;
  event.view_id = view_id;
  event.phase = FlutterPointerPhase::kDown;
  event.struct_size = sizeof(event);

  self->embedder_api.SendPointerEvent(self->engine, &event, 1);
}

void fl_engine_send_touch_move_event(FlEngine* self,
                                     FlutterViewId view_id,
                                     size_t timestamp,
                                     double x,
                                     double y,
                                     int32_t device) {
  g_return_if_fail(FL_IS_ENGINE(self));

  if (self->engine == nullptr) {
    return;
  }

  FlutterPointerEvent event;
  event.timestamp = timestamp;
  event.x = x;
  event.y = y;
  event.device_kind = kFlutterPointerDeviceKindTouch;
  event.device = device;
  event.buttons = FlutterPointerMouseButtons::kFlutterPointerButtonMousePrimary;
  event.view_id = view_id;
  event.phase = FlutterPointerPhase::kMove;
  event.struct_size = sizeof(event);

  self->embedder_api.SendPointerEvent(self->engine, &event, 1);
}

void fl_engine_send_touch_add_event(FlEngine* self,
                                    FlutterViewId view_id,
                                    size_t timestamp,
                                    double x,
                                    double y,
                                    int32_t device) {
  g_return_if_fail(FL_IS_ENGINE(self));

  if (self->engine == nullptr) {
    return;
  }

  FlutterPointerEvent event;
  event.timestamp = timestamp;
  event.x = x;
  event.y = y;
  event.device_kind = kFlutterPointerDeviceKindTouch;
  event.device = device;
  event.buttons = 0;
  event.view_id = view_id;
  event.phase = FlutterPointerPhase::kAdd;
  event.struct_size = sizeof(event);

  self->embedder_api.SendPointerEvent(self->engine, &event, 1);
}

void fl_engine_send_touch_remove_event(FlEngine* self,
                                       FlutterViewId view_id,
                                       size_t timestamp,
                                       double x,
                                       double y,
                                       int32_t device) {
  g_return_if_fail(FL_IS_ENGINE(self));

  if (self->engine == nullptr) {
    return;
  }

  FlutterPointerEvent event;
  event.timestamp = timestamp;
  event.x = x;
  event.y = y;
  event.device_kind = kFlutterPointerDeviceKindTouch;
  event.device = device;
  event.buttons = 0;
  event.view_id = view_id;
  event.phase = FlutterPointerPhase::kRemove;
  event.struct_size = sizeof(event);

  self->embedder_api.SendPointerEvent(self->engine, &event, 1);
}

void fl_engine_send_pointer_pan_zoom_event(FlEngine* self,
                                           FlutterViewId view_id,
                                           size_t timestamp,
                                           double x,
                                           double y,
                                           FlutterPointerPhase phase,
                                           double pan_x,
                                           double pan_y,
                                           double scale,
                                           double rotation) {
  g_return_if_fail(FL_IS_ENGINE(self));

  if (self->engine == nullptr) {
    return;
  }

  FlutterPointerEvent fl_event = {};
  fl_event.struct_size = sizeof(fl_event);
  fl_event.timestamp = timestamp;
  fl_event.x = x;
  fl_event.y = y;
  fl_event.phase = phase;
  fl_event.pan_x = pan_x;
  fl_event.pan_y = pan_y;
  fl_event.scale = scale;
  fl_event.rotation = rotation;
  fl_event.device = kPointerPanZoomDeviceId;
  fl_event.device_kind = kFlutterPointerDeviceKindTrackpad;
  fl_event.view_id = view_id;
  self->embedder_api.SendPointerEvent(self->engine, &fl_event, 1);
}

static void send_key_event_cb(bool handled, void* user_data) {
  g_autoptr(GTask) task = G_TASK(user_data);
  gboolean* return_value = g_new0(gboolean, 1);
  *return_value = handled;
  g_task_return_pointer(task, return_value, g_free);
}

void fl_engine_send_key_event(FlEngine* self,
                              const FlutterKeyEvent* event,
                              GCancellable* cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data) {
  g_return_if_fail(FL_IS_ENGINE(self));

  g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);

  if (self->engine == nullptr) {
    g_task_return_new_error(task, fl_engine_error_quark(),
                            FL_ENGINE_ERROR_FAILED, "No engine");
    return;
  }

  if (self->embedder_api.SendKeyEvent(self->engine, event, send_key_event_cb,
                                      g_object_ref(task)) != kSuccess) {
    g_task_return_new_error(task, fl_engine_error_quark(),
                            FL_ENGINE_ERROR_FAILED, "Failed to send key event");
    g_object_unref(task);
  }
}

gboolean fl_engine_send_key_event_finish(FlEngine* self,
                                         GAsyncResult* result,
                                         gboolean* handled,
                                         GError** error) {
  g_return_val_if_fail(FL_IS_ENGINE(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  g_autofree gboolean* return_value =
      static_cast<gboolean*>(g_task_propagate_pointer(G_TASK(result), error));
  if (return_value == nullptr) {
    return FALSE;
  }

  *handled = *return_value;
  return TRUE;
}

void fl_engine_dispatch_semantics_action(FlEngine* self,
                                         FlutterViewId view_id,
                                         uint64_t node_id,
                                         FlutterSemanticsAction action,
                                         GBytes* data) {
  g_return_if_fail(FL_IS_ENGINE(self));

  if (self->engine == nullptr) {
    return;
  }

  const uint8_t* action_data = nullptr;
  size_t action_data_length = 0;
  if (data != nullptr) {
    action_data = static_cast<const uint8_t*>(
        g_bytes_get_data(data, &action_data_length));
  }

  FlutterSendSemanticsActionInfo info;
  info.struct_size = sizeof(FlutterSendSemanticsActionInfo);
  info.view_id = view_id;
  info.node_id = node_id;
  info.action = action;
  info.data = action_data;
  info.data_length = action_data_length;
  self->embedder_api.SendSemanticsAction(self->engine, &info);
}

gboolean fl_engine_mark_texture_frame_available(FlEngine* self,
                                                int64_t texture_id) {
  g_return_val_if_fail(FL_IS_ENGINE(self), FALSE);
  return self->embedder_api.MarkExternalTextureFrameAvailable(
             self->engine, texture_id) == kSuccess;
}

gboolean fl_engine_register_external_texture(FlEngine* self,
                                             int64_t texture_id) {
  g_return_val_if_fail(FL_IS_ENGINE(self), FALSE);
  return self->embedder_api.RegisterExternalTexture(self->engine, texture_id) ==
         kSuccess;
}

gboolean fl_engine_unregister_external_texture(FlEngine* self,
                                               int64_t texture_id) {
  g_return_val_if_fail(FL_IS_ENGINE(self), FALSE);
  return self->embedder_api.UnregisterExternalTexture(self->engine,
                                                      texture_id) == kSuccess;
}

G_MODULE_EXPORT FlBinaryMessenger* fl_engine_get_binary_messenger(
    FlEngine* self) {
  g_return_val_if_fail(FL_IS_ENGINE(self), nullptr);
  return self->binary_messenger;
}

FlTaskRunner* fl_engine_get_task_runner(FlEngine* self) {
  g_return_val_if_fail(FL_IS_ENGINE(self), nullptr);
  return self->task_runner;
}

void fl_engine_execute_task(FlEngine* self, FlutterTask* task) {
  g_return_if_fail(FL_IS_ENGINE(self));
  self->embedder_api.RunTask(self->engine, task);
}

G_MODULE_EXPORT FlTextureRegistrar* fl_engine_get_texture_registrar(
    FlEngine* self) {
  g_return_val_if_fail(FL_IS_ENGINE(self), nullptr);
  return self->texture_registrar;
}

void fl_engine_update_accessibility_features(FlEngine* self, int32_t flags) {
  g_return_if_fail(FL_IS_ENGINE(self));

  if (self->engine == nullptr) {
    return;
  }

  self->embedder_api.UpdateAccessibilityFeatures(
      self->engine, static_cast<FlutterAccessibilityFeature>(flags));
}

void fl_engine_request_app_exit(FlEngine* self) {
  g_return_if_fail(FL_IS_ENGINE(self));
  fl_platform_handler_request_app_exit(self->platform_handler);
}

FlWindowingHandler* fl_engine_get_windowing_handler(FlEngine* self) {
  g_return_val_if_fail(FL_IS_ENGINE(self), nullptr);
  return self->windowing_handler;
}

FlKeyboardManager* fl_engine_get_keyboard_manager(FlEngine* self) {
  g_return_val_if_fail(FL_IS_ENGINE(self), nullptr);
  return self->keyboard_manager;
}

FlTextInputHandler* fl_engine_get_text_input_handler(FlEngine* self) {
  g_return_val_if_fail(FL_IS_ENGINE(self), nullptr);
  return self->text_input_handler;
}

FlMouseCursorHandler* fl_engine_get_mouse_cursor_handler(FlEngine* self) {
  g_return_val_if_fail(FL_IS_ENGINE(self), nullptr);
  return self->mouse_cursor_handler;
}
