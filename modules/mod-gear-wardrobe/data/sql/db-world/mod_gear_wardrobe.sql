-- mod-gear-wardrobe 白名单表（可选）
-- GearWardrobe.EnableWhitelist = 1 时，客户端只能装备 entry 在此表里的物品。
-- 表结构按 AC 的 `data/sql/updates/pending_db_world/` 流程执行；本文件是安装副本。

CREATE TABLE IF NOT EXISTS `mod_gear_wardrobe_whitelist` (
  `entry` INT UNSIGNED NOT NULL,
  `slot` TINYINT UNSIGNED NOT NULL DEFAULT 255 COMMENT '0..18 装备槽；255 = 按 InventoryType 自动',
  `class_mask` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = 不限制；否则 AC class mask',
  `source` VARCHAR(16) NOT NULL DEFAULT '' COMMENT 'pvp / raid',
  PRIMARY KEY (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- 示例：放入你确认过的 80 级 PvP/团本装备 entry。
-- INSERT INTO `mod_gear_wardrobe_whitelist` (`entry`, `slot`, `class_mask`, `source`)
-- VALUES (40405, 4, 0, 'pvp');
