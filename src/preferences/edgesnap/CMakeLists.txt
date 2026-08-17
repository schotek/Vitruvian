# Preferences panel for window edge snapping (app_server EdgeSnap listener).
# Author: Vláďa Janeček <vlada@janecek.cloud>
#
Application(EdgeSnap
	SOURCES
	EdgeSnap.cpp
	EdgeSnapWindow.cpp
	LIBS shared localestub
	RDEF EdgeSnap.rdef
)
UsePrivateHeaders(EdgeSnap app interface shared support)
