---@meta

---The global qtc object defined in the Lua plugin.
---@class qtc
Qtc = {}

---@class (exact) QtcPlugin
---@field Name string The name of the plugin.
---@field Version string The version of the plugin. (`major.minor.patch`)
---@field CompatVersion string The lowest previous version of the plugin that this one is compatible to. (`major.minor.patch`)
---@field Vendor string The vendor of the plugin.
---@field Category string The category of the plugin.
---@field Dependencies? QtcPluginDependency[] The dependencies of the plugin.
---@field Description? string A short one line description of the plugin.
---@field LongDescription? string A long description of the plugin. Can contain newlines.
---@field Url? string The url of the plugin.
---@field License? string The license text of the plugin.
---@field Revision? string The revision of the plugin.
---@field Copyright? string The copyright of the plugin.
---@field Experimental? boolean Whether the plugin is experimental or not. ( Default: true )
---@field DisabledByDefault? boolean Whether the plugin is disabled by default or not. ( Default: true )
---@field setup function The setup function of the plugin.
---@field hooks? Hooks The hooks of the plugin.
---@field Mimetypes? string XML MIME-info for registering additional or adapting built-in MIME types.
---@field JsonWizardPaths? string[] A list of paths relative to the plugin location or paths to the Qt resource system that are searched for template-based wizards.
QtcPlugin = {}

---@class QtcPluginDependency
---@field Name string The name of the dependency.
---@field Version string The version of the dependency. (`major.minor.patch`)
---@field Required boolean Whether the dependency is required or not.
QtcPluginDependency = {}


---@class EditorHooks
---@field documentOpened function function(document)
---@field documentClosed function function(document)
EditorHooks = {}

---@class Hooks
---@field editors? EditorHooks
Hooks = {}
