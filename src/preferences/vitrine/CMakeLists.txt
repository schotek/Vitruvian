# Preferences panel for the Vitrine compositor.
# Author: Vláďa Janeček <vlada@janecek.cloud>
#
Application(Vitrine_prefs
	SOURCES
	Vitrine.cpp
	VitrineWindow.cpp
	LIBS shared localestub
	RDEF Vitrine.rdef
)
# Installed as "VitrineSettings", NOT "Vitrine": the Deskbar labels every
# running application with its file name, so sharing the compositor's name
# put two identical "Vitrine" rows in the list (and made `pkill -x Vitrine`
# hit both). CamelCase matches the neighbours — FileTypes, DataTranslations,
# ScreenSaver.
set_target_properties(Vitrine_prefs PROPERTIES OUTPUT_NAME VitrineSettings)
UsePrivateHeaders(Vitrine_prefs app interface shared support)
# Localization: compiles data/catalogs/preferences/vitrine/*.catkeys into
# /system/data/locale/catalogs/<signature>/. The "System name" entry is what
# makes Tracker and the Deskbar show the translated application name.
DoCatalogs("x-vnd.Vitruvian-VitrinePrefs" preferences/vitrine)
