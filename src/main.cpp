#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <climits>
#include <fstream>
#include <sstream>
#include <vector>

using namespace geode::prelude;

// Formato del archivo de macro: una línea por evento -> "frame down player"
//   frame  = paso de física (m_currentProgress)
//   down   = 1 presiona, 0 suelta
//   player = 1 jugador 1, 2 jugador 2
struct Click {
	int frame;
	bool down;
	bool p1;
};

// ---------------------------------------------------------------
// Estado
// ---------------------------------------------------------------
static std::vector<Click> g_macro;  // eventos (ordenados por frame)
static size_t g_idx = 0;            // siguiente evento a reproducir
static int g_authEnd = 0;           // antes de este frame manda la macro y se bloquean los clicks reales
static int g_lastFrame = 0;
static bool g_needSeek = true;
static bool g_injecting = false;
static bool g_deathHandled = false;

// Estado del asistente de clicks (snap)
struct Pending { int frame; bool down; bool p1; };
static std::vector<Pending> g_pending; // clicks retenidos que se soltarán en su frame exacto
static int g_delay[2] = { 0, 0 };      // retraso aplicado al hold actual (P1, P2)

// Estado del corrector automático
static bool g_fixing = false;       // hay un click candidato en prueba
static bool g_fixP1 = true;
static int g_fixPress = 0;          // frame original del click culpable
static int g_fixRel = -1;           // frame original de su release (-1 si no hubo)
static int g_prevFrame = -1;        // frame del evento anterior al culpable
static int g_fixTry = 0;            // cuántos desplazamientos se han probado
static int g_curPress = -1;         // candidato actual (frames)
static int g_curRel = -1;

// ---------------------------------------------------------------
// Ajustes
// ---------------------------------------------------------------
static bool getBool(char const* k) { return Mod::get()->getSettingValue<bool>(k); }
static int getInt(char const* k) { return static_cast<int>(Mod::get()->getSettingValue<int64_t>(k)); }

static bool autoFixOn() { return getBool("auto-fix"); }
static bool snapOn() { return !autoFixOn() && getBool("snap"); }
static bool replayOn() { return !autoFixOn() && !snapOn() && getBool("enabled"); }
static bool recordOn() { return !autoFixOn() && !snapOn() && !getBool("enabled") && getBool("record"); }
static bool anyOn() { return autoFixOn() || replayOn() || recordOn(); }

static void say(std::string const& text) {
	log::info("{}", text);
	Notification::create(text, NotificationIcon::Info)->show();
}

// ---------------------------------------------------------------
// Archivo de macro
// ---------------------------------------------------------------
static std::filesystem::path macroPath() {
	return Mod::get()->getSaveDir() / Mod::get()->getSettingValue<std::string>("macro-name");
}

static bool loadMacro() {
	g_macro.clear();
	std::ifstream f(macroPath());
	if (!f) return false;
	int off = getInt("offset");
	std::string line;
	while (std::getline(f, line)) {
		if (line.empty() || line[0] == '#') continue;
		std::istringstream ss(line);
		int frame, down, player = 1;
		if (!(ss >> frame >> down)) continue;
		ss >> player;
		g_macro.push_back({ std::max(0, frame + off), down != 0, player != 2 });
	}
	std::stable_sort(g_macro.begin(), g_macro.end(),
		[](Click const& a, Click const& b) { return a.frame < b.frame; });
	return true;
}

static void saveMacro() {
	if (g_macro.empty()) return;
	std::ofstream f(macroPath());
	f << "# frame down player\n";
	for (auto& c : g_macro) {
		f << c.frame << ' ' << (c.down ? 1 : 0) << ' ' << (c.p1 ? 1 : 2) << '\n';
	}
	log::info("Macro guardada: {} eventos", g_macro.size());
}

// ---------------------------------------------------------------
// Utilidades de la macro
// ---------------------------------------------------------------
static void seek(int frame) {
	g_idx = std::lower_bound(g_macro.begin(), g_macro.end(), frame,
		[](Click const& c, int fr) { return c.frame < fr; }) - g_macro.begin();
}

static void eraseEvent(int frame, bool down, bool p1) {
	for (auto it = g_macro.begin(); it != g_macro.end(); ++it) {
		if (it->frame == frame && it->down == down && it->p1 == p1) {
			g_macro.erase(it);
			return;
		}
	}
}

static void insertEvent(Click c) {
	auto it = std::upper_bound(g_macro.begin(), g_macro.end(), c.frame,
		[](int fr, Click const& x) { return fr < x.frame; });
	g_macro.insert(it, c);
}

// Orden de prueba: +1, -1, +2, -2, +3, -3 ...
static int nextShift(int i) {
	int k = (i + 1) / 2;
	return (i % 2) ? k : -k;
}

// ---------------------------------------------------------------
// Corrector automático
// ---------------------------------------------------------------
// Coloca el click culpable desplazado 'shift' frames y hace que la macro
// mande hasta pasar la ventana de seguridad.
static bool tryCandidate(int shift) {
	int win = getInt("safe-window");
	int p = g_fixPress + shift;
	if (p < 0 || p <= g_prevFrame) return false;

	int rel = (g_fixRel >= 0) ? g_fixRel + shift : p + win - 1;
	if (rel <= p) rel = p + 1;

	if (g_curPress >= 0) {
		eraseEvent(g_curPress, true, g_fixP1);
		eraseEvent(g_curRel, false, g_fixP1);
	}
	insertEvent({ p, true, g_fixP1 });
	insertEvent({ rel, false, g_fixP1 });
	g_curPress = p;
	g_curRel = rel;
	g_authEnd = ((g_fixRel >= 0) ? rel : p) + win;
	g_fixing = true;
	return true;
}

static void onDeath(int fd) {
	int win = getInt("safe-window");
	int maxTry = 2 * getInt("max-shift");

	// 1) Estaba probando un candidato y volvió a morir: probar el siguiente
	if (g_fixing && fd < g_authEnd) {
		while (++g_fixTry <= maxTry) {
			int s = nextShift(g_fixTry);
			if (tryCandidate(s)) {
				say(fmt::format("Probando click con {:+} frames", s));
				return;
			}
		}
		// Se acabaron los intentos: quita el candidato y devuelve el control
		eraseEvent(g_curPress, true, g_fixP1);
		eraseEvent(g_curRel, false, g_fixP1);
		g_curPress = g_curRel = -1;
		g_authEnd = g_fixPress;
		g_fixing = false;
		say("No encontré un frame que funcione para ese click");
		return;
	}
	g_fixing = false;

	// 2) Muerte nueva: el último click antes de morir es el sospechoso
	int ci = -1;
	for (int i = static_cast<int>(g_macro.size()) - 1; i >= 0; --i) {
		if (g_macro[i].down && g_macro[i].frame <= fd) {
			ci = i;
			break;
		}
	}
	if (ci < 0 || fd - g_macro[ci].frame > win) return; // no hubo click reciente que corregir

	Click c = g_macro[ci];
	g_fixP1 = c.p1;
	g_fixPress = c.frame;
	g_fixRel = -1;
	for (size_t j = ci + 1; j < g_macro.size(); ++j) {
		if (!g_macro[j].down && g_macro[j].p1 == c.p1) {
			g_fixRel = g_macro[j].frame;
			break;
		}
	}
	g_prevFrame = (ci > 0) ? g_macro[ci - 1].frame : -1;

	// Ese intento murió: descarta el click culpable y todo lo posterior
	g_macro.erase(g_macro.begin() + ci, g_macro.end());
	g_curPress = g_curRel = -1;
	g_fixTry = 0;

	while (++g_fixTry <= maxTry) {
		int s = nextShift(g_fixTry);
		if (tryCandidate(s)) {
			say(fmt::format("Corrigiendo click: probando {:+} frames", s));
			return;
		}
	}
	g_authEnd = g_fixPress; // sin candidatos válidos: juegas tú esa parte
}

// ---------------------------------------------------------------
// Asistente de clicks (snap)
// ---------------------------------------------------------------
// Compara tu click con la macro de referencia. Si lo diste ANTES del frame
// correcto y dentro de la ventana, lo retiene y lo suelta en el frame exacto.
// (No puede corregir clicks tardíos: no se puede volver atrás en el tiempo.)
// Devuelve true si el click original debe bloquearse.
static bool snapHandle(int frame, bool down, bool p1) {
	int W = getInt("snap-window");
	int idx = p1 ? 0 : 1;

	if (down) {
		g_delay[idx] = 0;
		for (auto& c : g_macro) { // g_macro = referencia, ordenada por frame
			if (!c.down || c.p1 != p1) continue;
			if (c.frame < frame) continue;          // referencia ya pasada
			if (c.frame == frame) return false;     // ya estaba exacto
			if (c.frame - frame <= W) {             // llegaste antes: se retiene
				g_pending.push_back({ c.frame, true, p1 });
				g_delay[idx] = c.frame - frame;
				return true;
			}
			break; // la siguiente referencia está fuera de la ventana
		}
		return false;
	}

	// Release: conserva el mismo tiempo de hold que diste
	int d = g_delay[idx];
	if (d > 0) {
		g_pending.push_back({ frame + d, false, p1 });
		g_delay[idx] = 0;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------
class $modify(FFGameLayer, GJBaseGameLayer) {
	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (snapOn()) {
			int frame = m_gameState.m_currentProgress;
			if (g_needSeek || frame < g_lastFrame) {
				g_needSeek = false;
				g_pending.clear();
				g_delay[0] = g_delay[1] = 0;
			}
			g_lastFrame = frame;

			// Suelta los clicks retenidos cuando llega su frame exacto
			for (size_t i = 0; i < g_pending.size();) {
				if (g_pending[i].frame <= frame) {
					auto c = g_pending[i];
					g_pending.erase(g_pending.begin() + i);
					g_injecting = true;
					this->handleButton(c.down, 1, c.p1);
					g_injecting = false;
				} else {
					++i;
				}
			}
		} else if (anyOn()) {
			int frame = m_gameState.m_currentProgress;

			// Reinicio o checkpoint: reposiciona y descarta lo que quedó del intento anterior
			if (g_needSeek || frame < g_lastFrame) {
				g_needSeek = false;
				while (!g_macro.empty() && g_macro.back().frame >= frame && g_macro.back().frame >= g_authEnd) {
					g_macro.pop_back();
				}
				seek(frame);
				// Si estamos a mitad de un hold de la macro, restáuralo
				if (g_idx > 0 && g_macro[g_idx - 1].down && g_macro[g_idx - 1].frame < g_authEnd) {
					g_injecting = true;
					this->handleButton(true, 1, g_macro[g_idx - 1].p1);
					g_injecting = false;
				}
			}
			g_lastFrame = frame;

			// Pasó la ventana de seguridad: el click candidato funciona
			if (g_fixing && frame >= g_authEnd) {
				g_fixing = false;
				say("Click corregido");
			}

			// Reproduce los eventos que manda la macro
			while (g_idx < g_macro.size() && g_macro[g_idx].frame <= frame && g_macro[g_idx].frame < g_authEnd) {
				auto c = g_macro[g_idx++];
				g_injecting = true;
				this->handleButton(c.down, 1, c.p1);
				g_injecting = false;
			}
		}
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		if (button == 1 && !g_injecting) {
			int frame = m_gameState.m_currentProgress;
			if (snapOn()) {
				if (snapHandle(frame, down, isPlayer1)) return;
			} else if (anyOn()) {
				if (frame < g_authEnd) return; // la macro manda: se ignoran tus clicks
				if (autoFixOn() || recordOn()) {
					g_macro.push_back({ frame, down, isPlayer1 });
				}
			}
		}
		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}
};

class $modify(FFPlayLayer, PlayLayer) {
	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

		g_macro.clear();
		g_idx = 0;
		g_lastFrame = 0;
		g_needSeek = true;
		g_deathHandled = false;
		g_fixing = false;
		g_curPress = g_curRel = -1;
		g_authEnd = 0;
		g_pending.clear();
		g_delay[0] = g_delay[1] = 0;

		if (replayOn() || snapOn()) {
			if (loadMacro()) {
				if (replayOn()) g_authEnd = INT_MAX; // la macro manda todo el nivel
				log::info("Macro cargada: {} eventos", g_macro.size());
			} else {
				log::warn("No se pudo abrir {}", macroPath().string());
				say("No encontré la macro de referencia");
			}
		}
		return true;
	}

	void resetLevel() {
		PlayLayer::resetLevel();
		g_needSeek = true;
		g_deathHandled = false;
	}

	void destroyPlayer(PlayerObject* player, GameObject* object) {
		if (autoFixOn() && !g_deathHandled) {
			g_deathHandled = true;
			onDeath(m_gameState.m_currentProgress);
		}
		PlayLayer::destroyPlayer(player, object);
	}

	void levelComplete() {
		if (autoFixOn() || recordOn()) saveMacro();
		PlayLayer::levelComplete();
	}

	void onQuit() {
		if (recordOn()) saveMacro();
		PlayLayer::onQuit();
	}
};
