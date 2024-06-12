-- Tags: no-parallel

DROP SETTINGS PROFILE IF EXISTS s1_01294, s2_01294, s3_01294, s4_01294, s5_01294, s6_01294, s7_01294, s8_01294, s9_01294, s10_01294;
DROP SETTINGS PROFILE IF EXISTS s2_01294_renamed;
DROP USER IF EXISTS u1_01294;
DROP ROLE IF EXISTS r1_01294;

SELECT '-- default';
CREATE SETTINGS PROFILE s1_01294;
SHOW CREATE SETTINGS PROFILE s1_01294;

SELECT '-- same as default';
CREATE SETTINGS PROFILE s2_01294 SETTINGS NONE TO NONE;
CREATE PROFILE s3_01294;
SHOW CREATE PROFILE s2_01294;
SHOW CREATE SETTINGS PROFILE s3_01294;

SELECT '-- rename';
ALTER SETTINGS PROFILE s2_01294 RENAME TO 's2_01294_renamed';
SHOW CREATE SETTINGS PROFILE s2_01294; -- { serverError THERE_IS_NO_PROFILE } -- Profile not found
SHOW CREATE SETTINGS PROFILE s2_01294_renamed;
DROP SETTINGS PROFILE s1_01294, s2_01294_renamed, s3_01294;

SELECT '-- settings';
CREATE PROFILE s1_01294 SETTINGS NONE;
CREATE PROFILE s2_01294 SETTINGS INHERIT 'default';
CREATE PROFILE s3_01294 SETTINGS max_memory_usage=5000000;
CREATE PROFILE s4_01294 SETTINGS max_memory_usage MIN=5000000;
CREATE PROFILE s5_01294 SETTINGS max_memory_usage MAX=5000000;
CREATE PROFILE s6_01294 SETTINGS max_memory_usage CONST;
CREATE PROFILE s7_01294 SETTINGS max_memory_usage WRITABLE;
CREATE PROFILE s8_01294 SETTINGS max_memory_usage=5000000 MIN 4000000 MAX 6000000 CONST;
CREATE PROFILE s9_01294 SETTINGS INHERIT 'default', max_memory_usage=5000000 WRITABLE;
CREATE PROFILE s10_01294 SETTINGS INHERIT s1_01294, s3_01294, INHERIT default, readonly=0, max_memory_usage MAX 6000000;
SHOW CREATE PROFILE s1_01294;
SHOW CREATE PROFILE s2_01294;
SHOW CREATE PROFILE s3_01294;
SHOW CREATE PROFILE s4_01294;
SHOW CREATE PROFILE s5_01294;
SHOW CREATE PROFILE s6_01294;
SHOW CREATE PROFILE s7_01294;
SHOW CREATE PROFILE s8_01294;
SHOW CREATE PROFILE s9_01294;
SHOW CREATE PROFILE s10_01294;
ALTER PROFILE s1_01294 SETTINGS readonly=0;
ALTER PROFILE s2_01294 SETTINGS readonly=1;
ALTER PROFILE s3_01294 SETTINGS NONE;
SHOW CREATE PROFILE s1_01294;
SHOW CREATE PROFILE s2_01294;
SHOW CREATE PROFILE s3_01294;
DROP PROFILE s1_01294, s2_01294, s3_01294, s4_01294, s5_01294, s6_01294, s7_01294, s8_01294, s9_01294, s10_01294;

SELECT '-- to roles';
CREATE ROLE r1_01294;
CREATE USER u1_01294;
CREATE PROFILE s1_01294 TO NONE;
CREATE PROFILE s2_01294 TO ALL;
CREATE PROFILE s3_01294 TO r1_01294;
CREATE PROFILE s4_01294 TO u1_01294;
CREATE PROFILE s5_01294 TO r1_01294, u1_01294;
CREATE PROFILE s6_01294 TO ALL EXCEPT r1_01294;
CREATE PROFILE s7_01294 TO ALL EXCEPT r1_01294, u1_01294;
SHOW CREATE PROFILE s1_01294;
SHOW CREATE PROFILE s2_01294;
SHOW CREATE PROFILE s3_01294;
SHOW CREATE PROFILE s4_01294;
SHOW CREATE PROFILE s5_01294;
SHOW CREATE PROFILE s6_01294;
SHOW CREATE PROFILE s7_01294;
ALTER PROFILE s1_01294 TO u1_01294;
ALTER PROFILE s2_01294 TO NONE;
SHOW CREATE PROFILE s1_01294;
SHOW CREATE PROFILE s2_01294;
DROP PROFILE s1_01294, s2_01294, s3_01294, s4_01294, s5_01294, s6_01294, s7_01294;

SELECT '-- complex';
CREATE SETTINGS PROFILE s1_01294 SETTINGS readonly=0 TO r1_01294;
SHOW CREATE SETTINGS PROFILE s1_01294;
ALTER SETTINGS PROFILE s1_01294 SETTINGS INHERIT 'default' TO NONE;
SHOW CREATE SETTINGS PROFILE s1_01294;
DROP SETTINGS PROFILE s1_01294;

SELECT '-- multiple profiles in one command';
CREATE PROFILE s1_01294, s2_01294 SETTINGS max_memory_usage=5000000;
CREATE PROFILE s3_01294, s4_01294 TO ALL;
SHOW CREATE PROFILE s1_01294, s2_01294, s3_01294, s4_01294;
ALTER PROFILE s1_01294, s2_01294 SETTINGS max_memory_usage=6000000;
SHOW CREATE PROFILE s1_01294, s2_01294, s3_01294, s4_01294;
ALTER PROFILE s2_01294, s3_01294, s4_01294 TO r1_01294;
SHOW CREATE PROFILE s1_01294, s2_01294, s3_01294, s4_01294;
DROP PROFILE s1_01294, s2_01294, s3_01294, s4_01294;

SELECT '-- readonly ambiguity';
CREATE PROFILE s1_01294 SETTINGS readonly=1;
CREATE PROFILE s2_01294 SETTINGS readonly readonly;
CREATE PROFILE s3_01294 SETTINGS profile readonly;
CREATE PROFILE s4_01294 SETTINGS profile readonly, readonly;
CREATE PROFILE s5_01294 SETTINGS profile readonly, readonly=1;
CREATE PROFILE s6_01294 SETTINGS profile readonly, readonly readonly;
SHOW CREATE PROFILE s1_01294;
SHOW CREATE PROFILE s2_01294;
SHOW CREATE PROFILE s3_01294;
SHOW CREATE PROFILE s4_01294;
SHOW CREATE PROFILE s5_01294;
SHOW CREATE PROFILE s6_01294;
DROP PROFILE s1_01294, s2_01294, s3_01294, s4_01294, s5_01294, s6_01294;

SELECT '-- system.settings_profiles';
CREATE PROFILE s1_01294;
CREATE PROFILE s2_01294 SETTINGS readonly=0 TO r1_01294;;
CREATE PROFILE s3_01294 SETTINGS max_memory_usage=5000000 MIN 4000000 MAX 6000000 CONST TO r1_01294;
CREATE PROFILE s4_01294 SETTINGS max_memory_usage=5000000 TO r1_01294;
CREATE PROFILE s5_01294 SETTINGS INHERIT default, readonly=0, max_memory_usage MAX 6000000 WRITABLE TO u1_01294;
CREATE PROFILE s6_01294 TO ALL EXCEPT u1_01294, r1_01294;
SELECT name, storage, num_elements, apply_to_all, apply_to_list, apply_to_except FROM system.settings_profiles WHERE name LIKE 's%\_01294' ORDER BY name;

SELECT '-- system.settings_profile_elements';
SELECT * FROM system.settings_profile_elements WHERE profile_name LIKE 's%\_01294' ORDER BY profile_name, index;
DROP PROFILE s1_01294, s2_01294, s3_01294, s4_01294, s5_01294, s6_01294;

DROP ROLE r1_01294;
DROP USER u1_01294;
