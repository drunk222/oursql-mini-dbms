-- OurSQL frontend all-runnable SQL suite
-- Use a fresh database file when running this script.

CREATE TABLE demo_students(id INT, name VARCHAR(32), age INT, grade VARCHAR(8));
CREATE TABLE demo_scores(id INT, student_id INT, score INT);

INSERT INTO demo_students VALUES(1, 'Alice', 20, 'A');
INSERT INTO demo_students VALUES(2, 'Bob', 18, 'B');
INSERT INTO demo_students VALUES(3, 'Cara', 22, 'A');
INSERT INTO demo_students VALUES(4, 'Dora', 19, 'C');

INSERT INTO demo_scores VALUES(1, 1, 80);
INSERT INTO demo_scores VALUES(2, 3, 95);
INSERT INTO demo_scores VALUES(3, 2, 70);

CREATE INDEX idx_demo_students_age ON demo_students(age);
CREATE UNIQUE INDEX idx_demo_students_id ON demo_students(id);

EXPLAIN SELECT * FROM demo_students WHERE id = 1;

SELECT * FROM demo_students;
SELECT id, name FROM demo_students;
SELECT id AS student_id, name AS student_name FROM demo_students;
SELECT s.id FROM demo_students AS s;
SELECT DISTINCT grade FROM demo_students;

SELECT id FROM demo_students WHERE id = 1;
SELECT id FROM demo_students WHERE age >= 20 AND name LIKE 'A%';
SELECT id FROM demo_students WHERE id BETWEEN 1 AND 3;
SELECT id FROM demo_students WHERE id IN (1, 3);
SELECT id FROM demo_students WHERE grade IS NULL;
SELECT id FROM demo_students WHERE grade IS NOT NULL;
SELECT id FROM demo_students WHERE NOT name = 'Alice';

SELECT id FROM demo_students ORDER BY age DESC, id ASC;
SELECT * FROM demo_students LIMIT 2;
SELECT grade, COUNT(*) FROM demo_students GROUP BY grade HAVING COUNT(*) >= 1;
SELECT COUNT(*) FROM demo_students;
SELECT SUM(age), AVG(age), MAX(age), MIN(age) FROM demo_students;

SELECT ds.name AS student_name, sc.score
FROM demo_students AS ds
INNER JOIN demo_scores AS sc ON ds.id = sc.student_id;

UPDATE demo_students SET age = age + 1, grade = 'B' WHERE id = 4;
UPDATE demo_scores SET score = score + 5;
DELETE FROM demo_students WHERE id = 4;
DELETE FROM demo_scores WHERE id = 3;

BEGIN;
INSERT INTO demo_students VALUES(5, 'Eve', 22, 'C');
ROLLBACK;

BEGIN;
INSERT INTO demo_students VALUES(6, 'Frank', 23, 'A');
COMMIT;

DROP INDEX idx_demo_students_age;
DROP INDEX idx_demo_students_id;
DROP TABLE demo_scores;
DROP TABLE demo_students;
