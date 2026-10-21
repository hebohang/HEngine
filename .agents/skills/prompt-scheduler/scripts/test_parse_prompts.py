import unittest

from parse_prompts import parse


class ParsePromptsTests(unittest.TestCase):
    def test_explicit_preserves_body(self):
        result = parse("$prompt-scheduler\r\n【任务 1】\r\n甲\r\n\r\n【任务 2】\r\n乙")
        self.assertEqual([t["prompt"] for t in result["tasks"]], ["甲\r\n\r\n", "乙"])

    def test_implicit_and_single(self):
        self.assertEqual(len(parse("甲\n【任务 2】\n乙")["tasks"]), 2)
        self.assertEqual(parse("$prompt-scheduler\n甲")["tasks"][0]["prompt"], "甲")

    def test_code_and_inline_are_not_markers(self):
        for fence in ("```", "~~~~"):
            body = "提到【任务 9】\n" + fence + "text\n【任务 8】\n" + fence + "\n"
            self.assertEqual(parse(body + "【任务 2】\n乙")["tasks"][0]["prompt"], body)

    def test_longer_fence_requires_matching_close(self):
        body = "````\n```\n【任务 8】\n````\n"
        self.assertEqual(len(parse(body + "【任务 2】\n乙")["tasks"]), 2)

    def test_invalid_inputs(self):
        for raw in ("", "$prompt-scheduler\n", "【任务 1】\n", "甲\n【任务 3】\n乙",
                    "甲\n【任务 1】\n乙", "【任务 1】\n甲\n【任务 2】\n",
                    "甲\n【任务 02】\n乙", "```\n【任务 2】\n乙"):
            with self.subTest(raw=raw):
                with self.assertRaises(ValueError):
                    parse(raw)


if __name__ == "__main__":
    unittest.main()
