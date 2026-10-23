#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

using namespace geode::prelude;

// Formato del archivo: una línea por evento -> "frame down player"
//   frame  = paso de física (m_currentProgress)
//   down   = 1 presiona, 0 suelta
//   player = 1 jugador 1, 2 jugador 2
// Las líneas que empiezan con # son comentarios.
struct Click {
	int frame;
	bool down;
	bool p1;
};

static std::vector<Click> g_macro;    // macro cargada (reproducción)
static std::vector<Click> g_recorded; // macro en grabación
static size_t g_idx = 0;              // siguiente evento a reproducir
static int g_lastFrame = 0;
static bool g_injecting = false;

static bool replayOn() { return Mod::get()->getSettingValue<bool>("enabled"); }
static bool recordOn() { return !replayOn() && Mod::get()->getSettingValue<bool>("record"); }
static int offset() { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("offset")); }

static std::filesystem::path macroPath() {
	return Mod::get()->getSaveDir() / Mod::get()->getSettingValue<std::string>("macro-name");
}

static bool loadMacro() {
	g_macro.clear();
	std::ifstream f(macroPath());
	if (!f) return false;
	std::string line;
	while (std::getline(f, line)) {
		if (line.empty() || line[0] == '#') continue;
		std::istringstream ss(line);
		int frame, down, player = 1;
		if (!(ss >> frame >> down)) continue;
		ss >> player;
		g_macro.push_back({ frame, down != 0, player != 2 });
	}
	std::stable_sort(g_macro.begin(), g_macro.end(),
		[](Click const& a, Click const& b) { return a.frame < b.frame; });
	return true;
}

static void saveMacro() {
	if (g_recorded.empty()) return;
	std::ofstream f(macroPath());
	f << "# frame down player\n";
	for (auto& c : g_recorded) {
		f << c.frame << ' ' << (c.down ? 1 : 0) << ' ' << (c.p1 ? 1 : 2) << '\n';
	}
	log::info("Macro guardada: {} eventos", g_recorded.size());
}

// Coloca g_idx en el primer evento que aún no ha ocurrido en 'frame'
static void seek(int frame) {
	int off = offset();
	g_idx = std::lower_bound(g_macro.begin(), g_macro.end(), frame,
		[off](Click const& c, int fr) { return c.frame + off < fr; }) - g_macro.begin();
}

class $modify(FFGameLayer, GJBaseGameLayer) {
	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (replayOn() && !g_macro.empty()) {
			int frame = m_gameState.m_currentProgress;

			// Si el frame retrocedió (reinicio o checkpoint), reposiciona
			if (frame < g_lastFrame) {
				seek(frame);
				// Restaura el estado del botón si estamos a mitad de un hold
				if (g_idx > 0 && g_macro[g_idx - 1].down) {
					g_injecting = true;
					this->handleButton(true, 1, g_macro[g_idx - 1].p1);
					g_injecting = false;
				}
			}
			g_lastFrame = frame;

			while (g_idx < g_macro.size() && g_macro[g_idx].frame + offset() <= frame) {
				auto& c = g_macro[g_idx++];
				g_injecting = true;
				this->handleButton(c.down, 1, c.p1);
				g_injecting = false;
			}
		}
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		// Durante la reproducción solo pasan los clicks de la macro
		if (replayOn() && !g_injecting) return;

		if (recordOn()) {
			int frame = m_gameState.m_currentProgress;
			// Si volvimos a un checkpoint, descarta lo grabado después de ese punto
			while (!g_recorded.empty() && g_recorded.back().frame > frame) {
				g_recorded.pop_back();
			}
			g_recorded.push_back({ frame, down, isPlayer1 });
		}
		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}
};

class $modify(FFPlayLayer, PlayLayer) {
	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
		g_recorded.clear();
		g_idx = 0;
		g_lastFrame = 0;
		if (replayOn()) {
			if (!loadMacro()) {
				log::warn("No se pudo abrir {}", macroPath().string());
			} else {
				log::info("Macro cargada: {} eventos", g_macro.size());
			}
		}
		return true;
	}

	void resetLevel() {
		PlayLayer::resetLevel();
		if (replayOn()) {
			g_lastFrame = 0;
			seek(0);
		}
	}

	void levelComplete() {
		if (recordOn()) saveMacro();
		PlayLayer::levelComplete();
	}

	void onQuit() {
		if (recordOn()) saveMacro();
		PlayLayer::onQuit();
	}
};
