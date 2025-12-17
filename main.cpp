#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <mutex>
#include <raylib.h>

// ======================================================
//                     ESTRUCTURAS
// ======================================================

struct Laser {
    Vector2 position;
    bool active;
};

struct Spaceship {
    Texture2D image;
    Vector2 position;
    float speed;
    
    // Helper para obtener el rectángulo de colisión fácilmente
    Rectangle getRect() {
        return { position.x, position.y, (float)image.width, (float)image.height };
    }
};

struct Enemy {
    Texture2D image;
    Vector2 position;
    bool alive;
    
    // Helper para obtener el rectángulo de colisión fácilmente
    Rectangle getRect() {
        return { position.x, position.y, (float)image.width, (float)image.height };
    }
};

// ======================================================
//        VARIABLES GLOBALES Y SINCRONIZACIÓN
// ======================================================

std::atomic<bool> running(false);

// Mutex para proteger la memoria compartida entre hilos
std::mutex lasers_mutex;
std::mutex enemies_mutex;
std::mutex spaceship_mutex;

// Variables atómicas para inputs (Evita el lag de Windows)
std::atomic<int> input_move_dir(0); 
std::atomic<bool> input_fire(false);

Spaceship spaceship;
std::vector<Laser> lasers;
std::vector<Enemy> enemies;

// Array global para texturas de aliens (Para reutilizar en bucle infinito)
Texture2D alienTextures[3]; 

std::atomic<int> score = 0;
std::atomic<int> lives = 3; // Variable global para las vidas
int highscore = 0;

float enemySpeed = 2.0f;
int enemyDirection = 1;

// Variable para manejar la música de fondo
Music bgMusic;

enum GameScreen { MENU, GAMEPLAY, INSTRUCTIONS, HIGHSCORE, GAMEOVER };
GameScreen currentScreen = MENU;

// ======================================================
//                    FUNCIONES DEL JUEGO
// ======================================================

// Función auxiliar para generar una oleada de enemigos
// NOTA: Debe llamarse ya teniendo el candado enemies_mutex bloqueado
void SpawnWave() {
    enemies.clear(); // Limpiamos los enemigos viejos/muertos

    int columns = 6;
    int rows = 3; 
    int startX = 80;
    int startY = 60;
    int spacingX = 100;
    int spacingY = 60;

    for (int row = 0; row < rows; row++) {
        for (int i = 0; i < columns; i++) {
            Enemy e;
            int sel = (i + row) % 3;
            
            // Asignamos la textura desde el array global
            e.image = alienTextures[sel]; 
            
            e.position = { (float)(startX + i * spacingX), (float)(startY + row * spacingY) };
            e.alive = true;
            enemies.push_back(e);
        }
    }
}

void InitGame() {
    // Bloqueamos todo para reiniciar el juego de forma segura
    std::lock_guard<std::mutex> laser_lock(lasers_mutex);
    std::lock_guard<std::mutex> enemy_lock(enemies_mutex);
    std::lock_guard<std::mutex> ship_lock(spaceship_mutex);

    lasers.clear();
    // enemies.clear(); // Se hace dentro de SpawnWave
    score = 0;
    lives = 3; // Reiniciamos las vidas a 3 al empezar

    // Carga segura de la imagen de la nave
    if (spaceship.image.id == 0) spaceship.image = LoadTexture("Graphics/spaceship.png");
    
    // Si no encuentra la imagen, crea un cuadrado morado para que no truene
    if (spaceship.image.id == 0) {
        Image img = GenImageColor(50, 50, PURPLE);
        spaceship.image = LoadTextureFromImage(img);
        UnloadImage(img);
    }

    spaceship.position = { 
        (float)(GetScreenWidth()/2 - spaceship.image.width/2),
        (float)(GetScreenHeight() - spaceship.image.height - 10)
    };
    spaceship.speed = 5.0f;

    // Cargamos las texturas de aliens SOLO UNA VEZ en variables globales
    if (alienTextures[0].id == 0) {
        alienTextures[0] = LoadTexture("Graphics/alien_1.png");
        alienTextures[1] = LoadTexture("Graphics/alien_2.png");
        alienTextures[2] = LoadTexture("Graphics/alien_3.png");
    }

    // Reiniciamos velocidad y dirección
    enemySpeed = 2.0f;
    enemyDirection = 1;

    // Generamos la primera oleada
    SpawnWave();
}

void UnloadGameAssets() {
    UnloadTexture(spaceship.image);
    // Descargamos las texturas globales
    for (int i = 0; i < 3; i++) {
        UnloadTexture(alienTextures[i]);
    }
}

// Funciones de movimiento protegidas por Mutex
void MoveSpaceshipLeft() {
    std::lock_guard<std::mutex> lock(spaceship_mutex);
    if (spaceship.position.x > 0) spaceship.position.x -= spaceship.speed;
}

void MoveSpaceshipRight() {
    std::lock_guard<std::mutex> lock(spaceship_mutex);
    if (spaceship.position.x + spaceship.image.width < GetScreenWidth()) spaceship.position.x += spaceship.speed;
}

void FireLaser() {
    Laser laser;
    {
        // Obtenemos posición de la nave de forma segura
        std::lock_guard<std::mutex> lock(spaceship_mutex);
        laser.position = { spaceship.position.x + spaceship.image.width/2.0f - 2, spaceship.position.y };
    }
    laser.active = true;
    std::lock_guard<std::mutex> lock(lasers_mutex);
    lasers.push_back(laser);
}

// ======================================================
//                       HILOS
// ======================================================

void PlayerControl() {
    while (running) {
        // Leemos variables atómicas en vez de IsKeyDown directo
        int dir = input_move_dir.load();
        bool fire = input_fire.load();

        if (dir == -1) MoveSpaceshipLeft();
        if (dir == 1) MoveSpaceshipRight();
        
        if (fire) {
            FireLaser();
            input_fire = false; 
            std::this_thread::sleep_for(std::chrono::milliseconds(250)); // Cadencia de disparo
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void LaserUpdate() {
    while (running) {
        {
            std::lock_guard<std::mutex> laser_lock(lasers_mutex);
            std::lock_guard<std::mutex> enemy_lock(enemies_mutex);

            for (auto &laser : lasers) {
                if (!laser.active) continue;

                laser.position.y -= 10;
                if (laser.position.y < 0) { laser.active = false; continue; }

                for (auto &enemy : enemies) {
                    if (!enemy.alive) continue;
                    
                    // Colisión Laser vs Enemigo
                    Rectangle rLaser = { laser.position.x, laser.position.y, 4, 15 };
                    if (CheckCollisionRecs(rLaser, enemy.getRect())) {
                        laser.active = false;
                        enemy.alive = false;
                        score++;
                        break;
                    }
                }
            }
            // Borrar lasers inactivos
            lasers.erase(std::remove_if(lasers.begin(), lasers.end(), [](const Laser &l){ return !l.active; }), lasers.end());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// ======================================================
//               LOGICA DE ENEMIGOS (Main Thread)
// ======================================================

// Función que mueve enemigos y revisa si te mataron
// Devuelve true si es GAME OVER
bool UpdateEnemiesAndCheckCollisions() {
    std::lock_guard<std::mutex> lock(enemies_mutex);
    bool touchEdge = false;
    bool playerHit = false;
    int activeEnemies = 0; // Contador para saber si limpiamos la oleada

    Rectangle shipRect;
    {
        std::lock_guard<std::mutex> shipLock(spaceship_mutex);
        shipRect = spaceship.getRect();
    }

    for (auto &e : enemies) {
        if (!e.alive) continue;
        
        activeEnemies++; // Contamos enemigo vivo
        e.position.x += enemySpeed * enemyDirection;

        // Rebote en las paredes
        if (e.position.x < 10 || e.position.x + e.image.width > GetScreenWidth() - 10) touchEdge = true;

        // Si enemigo choca con nave O llega al fondo
        if (CheckCollisionRecs(e.getRect(), shipRect) || e.position.y > GetScreenHeight()) {
            playerHit = true;
            e.alive = false; // El enemigo explota al chocar
        }
    }

    if (touchEdge) {
        enemyDirection *= -1;
        for (auto &e : enemies) e.position.y += 10;
    }

    // Si matamos a todos los enemigos, generamos nueva oleada más rápida
    if (activeEnemies == 0) {
        enemySpeed += 2.0f; // Aumentamos dificultad
        enemyDirection *= -1; // Cambiamos lado de entrada
        SpawnWave(); // Generamos nuevos aliens (Ya tenemos el mutex bloqueado)
    }

    // Lógica de vidas
    if (playerHit) {
        lives--;
    }

    if (lives <= 0) return true; // Se acabaron las vidas, GAME OVER

    return false;
}

// ======================================================
//                     PANTALLAS
// ======================================================

void DrawMenu() {
    DrawText("SPACIAL BLAST", 210, 100, 45, WHITE);
    DrawText("1. Jugar", 300, 250, 25, GREEN);
    DrawText("2. Instrucciones", 300, 300, 25, GREEN);
    DrawText("3. Highscore", 300, 350, 25, GREEN);
    DrawText("ESC para salir", 300, 450, 20, GRAY);
}

void DrawInstructions() {
    DrawText("INSTRUCCIONES", 250, 100, 30, YELLOW);
    DrawText("<-- / --> : Mover la nave", 200, 200, 20, WHITE);
    DrawText("ESPACIO : Disparar", 200, 230, 20, WHITE);
    DrawText("Tienes 3 vidas. Sobrevive!", 200, 300, 20, RED);
    DrawText("R  : Regresar al menu", 200, 400, 20, WHITE);
}

void DrawHighscore() {
    DrawText("HIGHSCORE", 280, 100, 30, SKYBLUE);
    DrawText(TextFormat("RECORD: %d", highscore), 250, 250, 40, YELLOW);
    DrawText("Presiona R para regresar", 230, 400, 20, GRAY);
}

// Función para dibujar la pantalla de derrota
void DrawGameOver() {
    // Dibujamos un fondo negro semi-transparente sobre el juego
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.85f));
    
    DrawText("GAME OVER", 240, 150, 50, RED);
    DrawText(TextFormat("Puntaje Final: %d", score.load()), 260, 250, 30, WHITE);
    
    if (score > highscore) {
        DrawText("¡NUEVO RECORD!", 290, 300, 20, YELLOW);
    }

    DrawText("ENTER - Reintentar", 260, 400, 25, GREEN);
    DrawText("M - Volver al Menu", 270, 450, 25, SKYBLUE);
}

// ======================================================
//                 HIGHSCORE ARCHIVO
// ======================================================

void SaveHighscore(int current_score) {
    if (current_score > highscore) {
        highscore = current_score;
        std::ofstream file("highscore.txt");
        if (file.is_open()) { file << highscore; file.close(); }
    }
}

void LoadHighscore() {
    std::ifstream file("highscore.txt");
    if (file.is_open()) { file >> highscore; file.close(); } 
    else highscore = 0;
}

// ======================================================
//                        MAIN
// ======================================================

int main() {
    InitWindow(750, 700, "Spacial Blast");
    
    // Inicializamos el dispositivo de audio
    InitAudioDevice(); 

    // Cargamos la música y le damos play en loop
    bgMusic = LoadMusicStream("Audio/Sounds_music.ogg");
    bgMusic.looping = true;
    PlayMusicStream(bgMusic); 

    SetTargetFPS(60);
    LoadHighscore();

    Color grey = {29, 29, 27, 255};

    std::thread controlThread;
    std::thread laserThread;

    while (!WindowShouldClose()) {
        
        //  Actualizamos el stream de música frame a frame
        UpdateMusicStream(bgMusic); 

        // INPUTS, Se leen aquí (hilo principal) y se mandan a los atomics
        if (currentScreen == GAMEPLAY) {
            int move = 0;
            if (IsKeyDown(KEY_LEFT)) move = -1;
            if (IsKeyDown(KEY_RIGHT)) move = 1;
            input_move_dir.store(move);
            if (IsKeyPressed(KEY_SPACE)) input_fire.store(true);
        }

        BeginDrawing();
        ClearBackground(grey);

        switch (currentScreen) {
        case MENU:
            DrawMenu();
            if (IsKeyPressed(KEY_ONE)) {
                InitGame();
                running = true;
                // Reiniciamos inputs
                input_move_dir = 0; input_fire = false;
                
                // Iniciamos hilos
                controlThread = std::thread(PlayerControl);
                laserThread = std::thread(LaserUpdate);
                currentScreen = GAMEPLAY;
            }
            else if (IsKeyPressed(KEY_TWO)) currentScreen = INSTRUCTIONS;
            else if (IsKeyPressed(KEY_THREE)) currentScreen = HIGHSCORE;
            else if (IsKeyPressed(KEY_ESCAPE)) CloseWindow();
            break;

        case INSTRUCTIONS:
            DrawInstructions();
            if (IsKeyPressed(KEY_R)) currentScreen = MENU;
            break;

        case HIGHSCORE:
            DrawHighscore();
            if (IsKeyPressed(KEY_R)) currentScreen = MENU;
            break;

        case GAMEPLAY:
            // Verificamos si perdemos vidas o Game Over
            if (UpdateEnemiesAndCheckCollisions()) {
                // GAME OVER DETECTADO
                running = false; // Paramos los hilos
                if (controlThread.joinable()) controlThread.join();
                if (laserThread.joinable()) laserThread.join();
                
                SaveHighscore(score.load());
                currentScreen = GAMEOVER; // Cambiamos a la pantalla de Game Over
            }

            // DIBUJADO DE ELEMENTOS (Protegido por Mutex)
            {
                std::lock_guard<std::mutex> lock(spaceship_mutex);
                DrawTextureV(spaceship.image, spaceship.position, WHITE);
            }
            {
                std::lock_guard<std::mutex> laser_lock(lasers_mutex);
                for (auto &laser : lasers) if (laser.active) DrawRectangle(laser.position.x, laser.position.y, 4, 15, BLUE);
            }
            {
                std::lock_guard<std::mutex> enemy_lock(enemies_mutex);
                for (auto &enemy : enemies) if (enemy.alive) DrawTextureV(enemy.image, enemy.position, WHITE);
            }

            // UI del juego
            DrawText(TextFormat("Puntaje: %d", score.load()), 20, 20, 20, YELLOW);
            // Dibujamos las vidas restantes
            DrawText(TextFormat("VIDAS: %d", lives.load()), 600, 20, 20, RED);

            // Tecla secreta para matar vidas (Debug)
            if (IsKeyPressed(KEY_K)) lives = 0; 
            break;

        case GAMEOVER:
            DrawGameOver(); //  Dibujamos Game Over
            
            if (IsKeyPressed(KEY_ENTER)) { // Reintentar
                InitGame(); // Resetea vidas y enemigos
                running = true;
                controlThread = std::thread(PlayerControl);
                laserThread = std::thread(LaserUpdate);
                currentScreen = GAMEPLAY;
            }
            else if (IsKeyPressed(KEY_M)) { // Menu
                currentScreen = MENU;
            }
            break;
        }

        EndDrawing();
    }

    // Limpieza al cerrar la ventana
    running = false;
    if (controlThread.joinable()) controlThread.join();
    if (laserThread.joinable()) laserThread.join();

    // Descarga de música y audio
    UnloadMusicStream(bgMusic);
    CloseAudioDevice(); 
    
    UnloadGameAssets();
    CloseWindow();

    return 0;
}