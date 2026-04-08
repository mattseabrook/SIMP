// game.h

#ifndef GAME_H
#define GAME_H

#include <map>
#include <functional>
#include <string>
#include <string_view>
#include <chrono>
#include <vector>
#include <thread>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#include <mmeapi.h>
#endif

#include "pvx.h"
#include "config.h"
#include "window.h"
#include "raycast.h"

/*
===============================================================================

	This header file defines the Game Engine Feature

===============================================================================
*/

//
// Animation state structure
//
struct AnimationState
{
	bool isPlaying = false;
	std::chrono::steady_clock::time_point lastFrameTime;
	size_t totalFrames = 0;

	void reset()
	{
		isPlaying = false;
		totalFrames = 0;
	}

	std::chrono::microseconds getFrameDuration(double currentFPS) const
	{
		return std::chrono::microseconds(static_cast<long long>(1000000.0 / currentFPS));
	}
};

//
// Structure that defines clickable areas
//
struct ClickArea
{
	float x;
	float y;
	float width;
	float height;
	int z_index = 0;
};

//
// Hotspot structure defining clickable areas
//
struct Hotspot
{
	ClickArea area;
	std::function<void()> action;
};

//
// Navigation points for moving between views
//
struct Navigation
{
	ClickArea area;
	std::string next_view;
};

//
// View structure for each camera/viewpoint
//
struct View
{
	std::vector<Hotspot> hotspots;
	std::vector<Navigation> navigations;
};

//
// Structure for grouping views together
//
struct ViewGroup
{
	std::vector<const char *> names; // e.g., {"f_1bb", "f_1fa"}
	View data;
};

///////////////////////////////////////////////////////////////////////////////
//     Per-frame timing state kept hot in cache
///////////////////////////////////////////////////////////////////////////////
struct FrameTiming
{
	std::chrono::steady_clock::time_point lastRenderTime{};
	bool dirtyFrame = true;
	double currentFPS = 24.0;
};

///////////////////////////////////////////////////////////////////////////////
//		Struct for managing game state
///////////////////////////////////////////////////////////////////////////////
struct GameState
{
	// Render mode selection (CPU SIMD vs GPU compute), with Auto heuristic
	enum class RenderMode
	{
		Auto = 0,
		CPU,
		GPU
	};

	//
	// SIMD selection for hot-path pixel conversion
	//
	enum class SIMDLevel
	{
		Scalar = 0,
		SSE2,
		SSSE3,
		AVX2
	};

	//
	// UI
	//
	struct UIState
	{
		bool enabled = false;
		int width = 0;
		int height = 0;
		std::vector<DisplayInfo> displays;
		int x = 0;
		int y = 0;
	} ui;

	//
	// Assets
	//
	std::string current_room;		// Current room identifier
	std::string previous_room;		// Previous room for change detection
	std::string current_view;		// Current view identifier
	std::string previous_view;		// Previous view for change detection

	//
	// 2D & FMV Graphics
	//
	PVXFile *currentPVX = nullptr;		  // Now points to PVXFile
	size_t currentFrameIndex = 0;		  // Current frame in currentPVX
	AnimationState animation;			  // Animation state management
	std::string transient_animation_name; // e.g., "dr_r"
	AnimationState transient_animation;	  // Playback state for transient
	size_t transient_frame_index = 0;	  // Current frame of transient
	PVXFile *transientPVX = nullptr;	  // Transient PVX

	std::vector<std::string> animation_sequence; // Stores the sequence of animations
	size_t animation_queue_index = 0;			 // Current position in the animation sequence
	std::function<void()> pending_action;		 // Optional action after the current animation

	const View *view = nullptr; // Current view object

	//
	// Rendering state (using FrameTiming struct for cache locality)
	//
	FrameTiming frameTiming;

	// Selected SIMD level for conversion paths
	SIMDLevel simd = SIMDLevel::Scalar;

	// Selected render mode (defaults populated from config)
	RenderMode renderMode = RenderMode::Auto;

	//
	// Main Menu
	//
	struct
	{
		bool enabled = false;
		const std::vector<uint8_t>* currentFrame = nullptr; // Zero-copy pointer to decoded frame
		size_t currentFrameIndex = 0;	   // Current frame in menu animation
		AnimationState animation;		   // Menu background animation state
		int selectedItem = 0;			   // 0=NEW GAME, 1=MULTIPLAYER, 2=OPTIONS, 3=EXIT
		bool itemClicked = false;		   // Track if an item was clicked
	} menu;

	//
	// Raycasting
	//
	struct
	{
		bool enabled = false;
		bool useMegatexture = false;
		bool showMapOverlay = false;
		float mapOverlayZoom = 4.0f;  // 1.0 = fit whole map, higher = zoomed in
		std::vector<uint8_t> exploredMap; // fog of war (1 = explored, 0 = hidden)
		RaycastPlayer player = {
			0.0f, 0.0f,		// Starting position
			0.0f,			// Initial angle
			deg2rad(90.0f), // Field of view (in radians)
			0.2f,			// Walk speed
			0.4f			// Run speed
		};
		const TileMap *map = nullptr; // Current map data (read-only)
	} raycast;

	//
	// Music
	//
	std::string current_song;								// Name of the currently playing song (e.g., "gu39")
	std::string transient_song;								// Transient song (if any)
	double main_song_position = 0.0;						// Position to resume from
	std::string music_mode;									// Playback mode: "opl2", "dual_opl2", "opl3"
	int midi_bank = 0;										// ADLMIDI built-in bank index
	std::thread music_thread;								// Thread for non-blocking music playback
	std::atomic<bool> music_playing{false};					// Flag to indicate if music is playing
	bool hasPlayedFirstSong = false;						// Tracks if any song has played yet
	bool is_transient_playing = false;						// Flag to check if transient is active
	float music_volume = 1.0f;								// Volume (0.0 to 1.0)
	std::vector<std::pair<std::string, double>> song_stack; // Previous songs

	//
	// PCM Audio
	//
	std::thread pcm_thread;	  // Thread for PCM playback
	std::atomic<bool> pcm_playing{false}; // Flag to indicate PCM status
#ifdef _WIN32
	HWAVEOUT pcm_handle = NULL; // Handle to currently playing device
#endif
};

//=============================================================================

extern GameState state;

//=============================================================================

// Function prototypes
const View *getView(std::string_view current_view);
void buildViewMap();
void viewHandler();
void maybeRenderFrame(bool force = false);
void init();

#endif // GAME_H
