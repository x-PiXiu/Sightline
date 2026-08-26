/**
 * @file game_plugin.h
 * @brief 游戏插件接口定义
 * @details 定义多游戏支持的插件架构，允许动态加载和扩展游戏类型
 * @author Game Server Team
 * @date 2026-02-23
 * @version 1.0.0
 */

#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>
#include <chrono>
#include <nlohmann/json.hpp>

namespace game_services {

/**
 * @brief 游戏状态枚举
 */
enum class GameState {
    WAITING,        // 等待玩家
    PLAYING,        // 游戏中
    PAUSED,         // 暂停
    FINISHED,       // 已结束
    ABORTED         // 已中止
};

/**
 * @brief 游戏结果枚举
 */
enum class GameResult {
    NONE,           // 未结束
    WIN,            // 胜利
    LOSS,           // 失败
    DRAW,           // 平局
    ABANDONED       // 弃权
};

/**
 * @brief 玩家信息结构
 */
struct PlayerInfo {
    std::string user_id;
    std::string display_name;
    std::string avatar_url;
    int rating = 1200;
    int team_id = 0;            // 队伍ID（用于团队游戏）
    nlohmann::json custom_data; // 游戏特定数据
};

/**
 * @brief 游戏动作结构
 */
struct GameAction {
    std::string action_type;    // 动作类型
    std::string player_id;      // 执行玩家
    int64_t timestamp = 0;      // 时间戳
    nlohmann::json data;        // 动作数据
};

/**
 * @brief 动作结果结构
 */
struct ActionResult {
    bool success = false;
    std::string error_message;
    nlohmann::json state_update;    // 状态更新
    std::vector<std::string> notify_players;  // 需要通知的玩家
    bool game_ended = false;
    std::string winner_id;
    GameResult result = GameResult::NONE;
};

/**
 * @brief 游戏配置结构
 */
struct GameConfig {
    std::string game_mode;
    int max_players = 2;
    int min_players = 2;
    bool allow_spectators = true;
    int time_limit_seconds = 0;     // 0表示无限制
    nlohmann::json custom_settings; // 游戏特定设置
};

/**
 * @brief 游戏元数据
 */
struct GameMetadata {
    std::string game_id;            // 游戏唯一标识
    std::string game_name;          // 游戏名称
    std::string game_version;       // 游戏版本
    std::string category;           // 游戏类别 (BOARD, CARD, ACTION, etc.)
    std::string description;        // 游戏描述
    std::string author;             // 作者
    std::string icon_url;           // 图标URL

    std::pair<int, int> player_range;  // 玩家数量范围
    bool supports_ai = false;       // 是否支持AI
    bool supports_teams = false;    // 是否支持组队

    std::vector<std::string> supported_modes;  // 支持的游戏模式
    nlohmann::json rating_config;   // 评分配置
    nlohmann::json reward_config;   // 奖励配置
};

/**
 * @brief 游戏回放数据
 */
struct GameReplay {
    std::string replay_id;
    std::string game_id;
    std::string game_type;

    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;
    int duration_seconds = 0;

    std::vector<PlayerInfo> players;
    std::vector<GameAction> actions;
    std::string winner_id;
    GameResult result = GameResult::NONE;

    nlohmann::json initial_state;   // 初始状态
    nlohmann::json final_state;     // 最终状态
    nlohmann::json metadata;        // 回放元数据

    /**
     * @brief 转换为JSON
     */
    nlohmann::json toJson() const {
        nlohmann::json players_json = nlohmann::json::array();
        for (const auto& p : players) {
            players_json.push_back({
                {"user_id", p.user_id},
                {"display_name", p.display_name},
                {"rating", p.rating}
            });
        }

        nlohmann::json actions_json = nlohmann::json::array();
        for (const auto& a : actions) {
            actions_json.push_back({
                {"action_type", a.action_type},
                {"player_id", a.player_id},
                {"timestamp", a.timestamp},
                {"data", a.data}
            });
        }

        return {
            {"replay_id", replay_id},
            {"game_id", game_id},
            {"game_type", game_type},
            {"duration_seconds", duration_seconds},
            {"players", players_json},
            {"actions", actions_json},
            {"winner_id", winner_id},
            {"result", static_cast<int>(result)},
            {"metadata", metadata}
        };
    }
};

/**
 * @brief 游戏插件接口
 * @details 所有游戏插件必须实现此接口
 */
class IGamePlugin {
public:
    virtual ~IGamePlugin() = default;

    // ========== 插件信息 ==========

    /**
     * @brief 获取插件ID
     */
    virtual std::string getPluginId() const = 0;

    /**
     * @brief 获取游戏元数据
     */
    virtual GameMetadata getMetadata() const = 0;

    /**
     * @brief 获取插件版本
     */
    virtual std::string getVersion() const = 0;

    // ========== 游戏生命周期 ==========

    /**
     * @brief 初始化插件
     * @param config 配置数据
     * @return 是否成功
     */
    virtual bool initialize(const nlohmann::json& config) = 0;

    /**
     * @brief 关闭插件
     */
    virtual void shutdown() = 0;

    /**
     * @brief 创建游戏实例
     * @param game_id 游戏实例ID
     * @param config 游戏配置
     * @return 是否成功
     */
    virtual bool createGame(const std::string& game_id, const GameConfig& config) = 0;

    /**
     * @brief 销毁游戏实例
     * @param game_id 游戏实例ID
     */
    virtual void destroyGame(const std::string& game_id) = 0;

    // ========== 玩家管理 ==========

    /**
     * @brief 添加玩家到游戏
     * @param game_id 游戏实例ID
     * @param player 玩家信息
     * @return 是否成功
     */
    virtual bool addPlayer(const std::string& game_id, const PlayerInfo& player) = 0;

    /**
     * @brief 从游戏移除玩家
     * @param game_id 游戏实例ID
     * @param player_id 玩家ID
     * @return 是否成功
     */
    virtual bool removePlayer(const std::string& game_id, const std::string& player_id) = 0;

    /**
     * @brief 获取游戏中的玩家列表
     * @param game_id 游戏实例ID
     * @return 玩家列表
     */
    virtual std::vector<PlayerInfo> getPlayers(const std::string& game_id) const = 0;

    /**
     * @brief 获取玩家数量
     * @param game_id 游戏实例ID
     * @return 玩家数量
     */
    virtual int getPlayerCount(const std::string& game_id) const = 0;

    // ========== 游戏逻辑 ==========

    /**
     * @brief 开始游戏
     * @param game_id 游戏实例ID
     * @return 是否成功
     */
    virtual bool startGame(const std::string& game_id) = 0;

    /**
     * @brief 暂停游戏
     * @param game_id 游戏实例ID
     * @return 是否成功
     */
    virtual bool pauseGame(const std::string& game_id) = 0;

    /**
     * @brief 恢复游戏
     * @param game_id 游戏实例ID
     * @return 是否成功
     */
    virtual bool resumeGame(const std::string& game_id) = 0;

    /**
     * @brief 结束游戏
     * @param game_id 游戏实例ID
     * @param reason 结束原因
     * @return 是否成功
     */
    virtual bool endGame(const std::string& game_id, const std::string& reason = "") = 0;

    /**
     * @brief 处理游戏动作
     * @param game_id 游戏实例ID
     * @param action 游戏动作
     * @return 动作结果
     */
    virtual ActionResult processAction(const std::string& game_id, const GameAction& action) = 0;

    /**
     * @brief 验证动作是否合法
     * @param game_id 游戏实例ID
     * @param action 游戏动作
     * @return 是否合法
     */
    virtual bool validateAction(const std::string& game_id, const GameAction& action) const = 0;

    // ========== 状态管理 ==========

    /**
     * @brief 获取游戏状态
     * @param game_id 游戏实例ID
     * @return 游戏状态
     */
    virtual GameState getGameState(const std::string& game_id) const = 0;

    /**
     * @brief 获取序列化的游戏状态
     * @param game_id 游戏实例ID
     * @return JSON格式的游戏状态
     */
    virtual nlohmann::json getSerializedState(const std::string& game_id) const = 0;

    /**
     * @brief 从序列化状态恢复游戏
     * @param game_id 游戏实例ID
     * @param state JSON格式的游戏状态
     * @return 是否成功
     */
    virtual bool restoreFromState(const std::string& game_id, const nlohmann::json& state) = 0;

    // ========== 回放系统 ==========

    /**
     * @brief 开始录制回放
     * @param game_id 游戏实例ID
     * @return 是否成功
     */
    virtual bool startRecording(const std::string& game_id) = 0;

    /**
     * @brief 停止录制并获取回放
     * @param game_id 游戏实例ID
     * @return 回放数据
     */
    virtual GameReplay stopRecording(const std::string& game_id) = 0;

    /**
     * @brief 从回放恢复游戏（用于回放功能）
     * @param replay 回放数据
     * @return 是否成功
     */
    virtual bool restoreFromReplay(const GameReplay& replay) = 0;

    // ========== AI 支持 ==========

    /**
     * @brief 是否支持AI玩家
     */
    virtual bool supportsAI() const { return false; }

    /**
     * @brief 添加AI玩家
     * @param game_id 游戏实例ID
     * @param difficulty AI难度（1-10）
     * @return AI玩家ID，失败返回空
     */
    virtual std::string addAIPlayer(const std::string& game_id, int difficulty = 5) {
        return "";
    }

    /**
     * @brief 获取AI推荐动作
     * @param game_id 游戏实例ID
     * @param ai_player_id AI玩家ID
     * @return 推荐的动作
     */
    virtual GameAction getAIAction(const std::string& game_id, const std::string& ai_player_id) {
        return GameAction{};
    }
};

/**
 * @brief 游戏插件工厂函数类型
 */
using GamePluginFactory = std::function<std::unique_ptr<IGamePlugin>()>;

/**
 * @brief 游戏插件注册表
 * @details 管理所有注册的游戏插件
 */
class GamePluginRegistry {
public:
    /**
     * @brief 获取单例实例
     */
    static GamePluginRegistry& getInstance() {
        static GamePluginRegistry instance;
        return instance;
    }

    /**
     * @brief 注册游戏插件
     * @param game_type 游戏类型标识
     * @param factory 插件工厂函数
     * @return 是否成功
     */
    bool registerPlugin(const std::string& game_type, GamePluginFactory factory) {
        if (factories_.find(game_type) != factories_.end()) {
            return false;  // 已存在
        }
        factories_[game_type] = std::move(factory);
        return true;
    }

    /**
     * @brief 注销游戏插件
     * @param game_type 游戏类型标识
     */
    void unregisterPlugin(const std::string& game_type) {
        factories_.erase(game_type);
    }

    /**
     * @brief 创建游戏插件实例
     * @param game_type 游戏类型标识
     * @return 插件实例，失败返回nullptr
     */
    std::unique_ptr<IGamePlugin> createPlugin(const std::string& game_type) {
        auto it = factories_.find(game_type);
        if (it == factories_.end()) {
            return nullptr;
        }
        return it->second();
    }

    /**
     * @brief 获取所有注册的游戏类型
     */
    std::vector<std::string> getRegisteredTypes() const {
        std::vector<std::string> types;
        for (const auto& pair : factories_) {
            types.push_back(pair.first);
        }
        return types;
    }

    /**
     * @brief 检查游戏类型是否已注册
     */
    bool hasPlugin(const std::string& game_type) const {
        return factories_.find(game_type) != factories_.end();
    }

private:
    GamePluginRegistry() = default;
    std::unordered_map<std::string, GamePluginFactory> factories_;
};

/**
 * @brief 游戏插件注册辅助类
 * @details 用于静态注册游戏插件
 */
template<typename T>
class GamePluginRegistrar {
public:
    explicit GamePluginRegistrar(const std::string& game_type) {
        GamePluginRegistry::getInstance().registerPlugin(game_type, []() {
            return std::make_unique<T>();
        });
    }
};

// 注册插件的宏
#define REGISTER_GAME_PLUGIN(PluginClass, game_type) \
    static game_services::GamePluginRegistrar<PluginClass> \
        _plugin_registrar_##PluginClass(game_type);

} // namespace game_services
