-- OurSQL 编译前端全功能可运行 SQL 测试脚本。
-- 建议使用全新的数据库文件运行本脚本。
-- 本脚本同时由 oursql_frontend_sql_tests 自动执行。

-- 1. 表结构定义与数据写入。
CREATE TABLE demo_students(
  id INT,
  name VARCHAR(32),
  age INT DEFAULT 18,
  grade VARCHAR(8) DEFAULT 'C'
);
CREATE TABLE demo_scores(id INT, student_id INT, score INT);
CREATE TABLE demo_courses(id INT, student_id INT, title VARCHAR(32));

-- 省略列时使用 DEFAULT，显式 NULL 用于验证 NULL Bitmap。
INSERT INTO demo_students(id, name) VALUES(1, 'Alice'), (2, 'Bob');
INSERT INTO demo_students(id, name, age, grade)
VALUES(3, 'Cara', 22, 'A'), (4, 'Dora', 19, 'C');
INSERT INTO demo_students VALUES(5, 'Eve', 20, NULL);

-- 全列单行和多行 INSERT。
INSERT INTO demo_scores VALUES(1, 1, 80), (2, 2, 70), (3, 3, 95), (4, 4, 60);
INSERT INTO demo_courses VALUES
  (1, 1, 'Math'), (2, 2, 'Physics'), (3, 3, 'Database');

-- 2. 索引创建与执行计划查看。
CREATE INDEX idx_demo_students_age ON demo_students(age);
CREATE UNIQUE INDEX idx_demo_students_id ON demo_students(id);

EXPLAIN SELECT * FROM demo_students WHERE id = 1;
EXPLAIN SELECT * FROM demo_students WHERE age = 20;

-- 3. 基础投影、别名、常量、DISTINCT 和 LIMIT。
SELECT * FROM demo_students;
SELECT id, name FROM demo_students;
SELECT id AS student_id, name AS student_name FROM demo_students;
SELECT s.id FROM demo_students AS s;
SELECT 1 AS one, 'ok' AS label, NULL AS missing
FROM demo_students
LIMIT 1;
SELECT age + 1 AS next_age, age - 1 AS previous_age,
       age * 2 AS double_age, age / 2 AS half_age
FROM demo_students
LIMIT 1;
SELECT (id + 1) * 2 AS computed_value
FROM demo_students
WHERE id = 1;
SELECT DISTINCT grade FROM demo_students;
SELECT * FROM demo_students LIMIT 2;

-- 4. WHERE 表达式。
SELECT id FROM demo_students WHERE id = 1;
SELECT id FROM demo_students WHERE age >= 20 AND name LIKE 'A%';
SELECT id FROM demo_students WHERE id BETWEEN 1 AND 3;
SELECT id FROM demo_students WHERE id IN (1, 3, 5);
SELECT id FROM demo_students WHERE grade IS NULL;
SELECT id FROM demo_students WHERE grade IS NOT NULL;
SELECT id FROM demo_students WHERE NOT name = 'Alice';
SELECT id FROM demo_students WHERE (id >= 2 AND age <= 22) OR grade = 'A';
SELECT id FROM demo_students WHERE age + 1 > 21;

-- 5. ORDER BY、GROUP BY、HAVING 和聚合函数。
SELECT id FROM demo_students ORDER BY age DESC, id ASC;
SELECT grade, COUNT(*) FROM demo_students GROUP BY grade HAVING COUNT(*) >= 1;
SELECT COUNT(*) FROM demo_students;
SELECT SUM(age), AVG(age), MAX(age), MIN(age) FROM demo_students;

-- 6. JOIN 执行：两表连接和左深三表连接。
SELECT ds.name AS student_name, sc.score
FROM demo_students AS ds
INNER JOIN demo_scores AS sc ON ds.id = sc.student_id;

SELECT ds.name AS student_name, sc.score
FROM demo_students AS ds
LEFT JOIN demo_scores AS sc ON ds.id = sc.student_id;

SELECT ds.name AS student_name, sc.score
FROM demo_students AS ds
RIGHT JOIN demo_scores AS sc ON ds.id = sc.student_id;

SELECT ds.name AS student_name, sc.score
FROM demo_students AS ds
FULL JOIN demo_scores AS sc ON ds.id = sc.student_id;

SELECT ds.name, sc.score, dc.title
FROM demo_students AS ds
JOIN demo_scores AS sc ON ds.id = sc.student_id
JOIN demo_courses AS dc ON dc.student_id = sc.student_id;

-- 7. 非相关标量、IN 和 EXISTS 子查询。
SELECT id
FROM demo_students
WHERE id IN (SELECT student_id FROM demo_scores WHERE score >= 80);

SELECT id
FROM demo_students
WHERE EXISTS (SELECT 1 FROM demo_scores WHERE score = 80);

SELECT (SELECT MAX(score) FROM demo_scores) AS max_score
FROM demo_students
LIMIT 1;

-- 8. UPDATE 和 DELETE。
UPDATE demo_students SET age = age + 1, grade = 'B' WHERE id = 4;
UPDATE demo_scores SET score = score + 5;
DELETE FROM demo_students WHERE id = 4;
DELETE FROM demo_scores WHERE id = 4;
DELETE FROM demo_courses WHERE id = 4;

-- 9. 显式事务回滚和提交。
BEGIN;
INSERT INTO demo_students VALUES(7, 'RolledBack', 30, 'D');
ROLLBACK;

BEGIN;
INSERT INTO demo_students VALUES(8, 'Committed', 24, 'A');
COMMIT;

SELECT id, name FROM demo_students WHERE id IN (7, 8);

-- 10. ALTER TABLE 重建数据、默认值和索引。
ALTER TABLE demo_students RENAME COLUMN grade TO grade_level;
ALTER TABLE demo_students ADD COLUMN active INT DEFAULT 1;
ALTER TABLE demo_students RENAME TO demo_learners;

SELECT id, name, age, grade_level, active
FROM demo_learners
ORDER BY id;

INSERT INTO demo_learners(id, name) VALUES(9, 'Grace');
SELECT id, name, age, grade_level, active
FROM demo_learners
WHERE id = 9;

EXPLAIN SELECT * FROM demo_learners WHERE id = 9;

-- 11. 清理数据。脚本测试要求最终不残留任何表。
DROP INDEX idx_demo_students_age;
DROP INDEX idx_demo_students_id;
DROP TABLE demo_courses;
DROP TABLE demo_scores;
DROP TABLE demo_learners;
