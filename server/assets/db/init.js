// init.js —— ep08 步骤 5：MongoDB 战报库初始化（文档库·best-effort）
// 执行：mongosh sightline < assets/db/init.js（或 db_up.sh 自动执行）
// 战报=一局一文档（§5.6 schema）；extra 空位约定（F1）：只放未来模式差异，不放现有字段副本。

db = db.getSiblingDB("sightline");

// 战报集合：_id=match_id；started_at 倒序索引=「最近 N 场」列表查询
db.createCollection("war_reports", { capped: false });
db.war_reports.createIndex({ started_at: -1 });
db.war_reports.createIndex({ config_version: 1 });

// 说明：kill_timeline 为变长数组（一杀一条），BSON 文档级原子写——
// 整份战报在 Ended 时一次性 insert（对局中只在内存缓冲，§5.2）。
print("sightline.war_reports ready");
