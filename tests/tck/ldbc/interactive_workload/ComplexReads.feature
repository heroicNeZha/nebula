# Copyright (c) 2021 vesoft inc. All rights reserved.
#
# This source code is licensed under Apache 2.0 License.
Feature: LDBC Interactive Workload - Complex Reads

  Background:
    Given a graph with space named "ldbc_v0_3_3"

  @skip
  Scenario: 1. Friends with certain name
    # TODO: shortestPath syntax
    When executing query:
      """
      MATCH p=shortestPath((person:Person)-[path:KNOWS*1..3]-(friend:Person {firstName: "Baruch
      WHERE id(person) == "person-4139"
      AND person <> friend
      WITH friend, length(p) AS distance
      ORDER BY distance ASC, friend.lastName ASC, toInteger(friend.id) ASC
      LIMIT 20
      MATCH (friend)-[:IS_LOCATED_IN]->(friendCity:Place)
      OPTIONAL MATCH (friend)-[studyAt:STUDY_AT]->(uni:Organisation)-[:IS_LOCATED_IN]->(uniCity:Place)
      WITH
        friend,
        collect(
          CASE uni.name
            WHEN null THEN null
            ELSE [uni.name, studyAt.classYear, uniCity.name]
          END
        ) AS unis,
        friendCity,
        distance
      OPTIONAL MATCH (friend)-[workAt:WORK_AT]->(company:Organisation)-[:IS_LOCATED_IN]->(companyCountry:Place)
      WITH
        friend,
        collect(
          CASE company.name
            WHEN null THEN null
            ELSE [company.name, workAt.workFrom, companyCountry.name]
          END
        ) AS companies,
        unis,
        friendCity,
        distance
      RETURN
        friend.id AS friendId,
        friend.lastName AS friendLastName,
        distance AS distanceFromPerson,
        friend.birthday AS friendBirthday,
        friend.creationDate AS friendCreationDate,
        friend.gender AS friendGender,
        friend.browserUsed AS friendBrowserUsed,
        friend.locationIP AS friendLocationIp,
        friend.email AS friendEmails,
        friend.speaks AS friendLanguages,
        friendCity.name AS friendCityName,
        unis AS friendUniversities,
        companies AS friendCompanies
      ORDER BY distanceFromPerson ASC, friendLastName ASC, toInteger(friendId) ASC
      LIMIT 20
      """
    Then a SyntaxError should be raised at runtime: syntax error near `shortestPath'

  Scenario: 2. Recent messages by your friends
    When executing query:
      """
      MATCH (n:Person)-[:KNOWS]-(friend:Person)<-[:HAS_CREATOR]-(message)
      WHERE id(n) == "person-4139" and
      CASE exists(message.Post.creationDate) 
        WHEN true THEN message.Post.creationDate <= "2011-03-12T18:26:25.436"
        ELSE message.Comment.creationDate <= "2011-03-12T18:26:25.436"
      END
      RETURN
        id(friend) AS personId,
        friend.Person.firstName AS personFirstName,
        friend.Person.lastName AS personLastName,
        id(message) AS messageId,
        CASE exists(message.Post.content) 
          WHEN true THEN 
            CASE message.Post.content != ""
              WHEN true THEN message.Post.content 
              ELSE message.Post.imageFile 
            END 
          ELSE message.`Comment`.content 
        END AS messageContent, 
        CASE exists(message.Post.creationDate) 
          WHEN true THEN message.Post.creationDate 
          ELSE message.`Comment`.creationDate
        END AS messageCreationDate
      ORDER BY messageCreationDate DESC, messageId ASC
      LIMIT 20
      """
    Then the result should be, in any order:
      | personId | personFirstName | personLastName | messageId | messageContent | messageCreationDate |

  @skip
  Scenario: 3. Friends and friends of friends that have been to given countries
    When executing query:
      # TODO: WHERE patterns
      """
      MATCH (person:Person)-[:KNOWS*1..2]-(friend:Person)<-[:HAS_CREATOR]-(messageX:Message),
      (messageX)-[:IS_LOCATED_IN]->(countryX:Place)
      WHERE
        id(person) == ""
        AND not(person==friend)
        AND not((friend)-[:IS_LOCATED_IN]->()-[:IS_PART_OF]->(countryX))
        AND countryX.name=$countryXName AND messageX.creationDate>=$startDate
        AND messageX.creationDate<$endDate
      WITH friend, count(DISTINCT messageX) AS xCount
      MATCH (friend)<-[:HAS_CREATOR]-(messageY:Message)-[:IS_LOCATED_IN]->(countryY:Place)
      WHERE
        countryY.name="$countryYName"
        AND not((friend)-[:IS_LOCATED_IN]->()-[:IS_PART_OF]->(countryY))
        AND messageY.creationDate>="$startDate"
        AND messageY.creationDate<"$endDate"
      WITH
        friend.id AS personId,
        friend.firstName AS personFirstName,
        friend.lastName AS personLastName,
        xCount,
        count(DISTINCT messageY) AS yCount
      RETURN
        personId,
        personFirstName,
        personLastName,
        xCount,
        yCount,
        xCount + yCount AS count
      ORDER BY count DESC, toInteger(personId) ASC
      LIMIT 20
      """
    Then a SyntaxError should be raised at runtime:

  Scenario: 4. New topics
    When executing query:
      """
      MATCH (person:Person)-[:KNOWS]-(:Person)<-[:HAS_CREATOR]-(post:Post)-[:HAS_TAG]->(`tag`:`Tag`)
      WHERE id(person) == "person-4139" AND post.Post.creationDate >= "2011-03-10"
         AND post.Post.creationDate < "2011-03-12"
      WITH person, count(post) AS postsOnTag, `tag`
      OPTIONAL MATCH (person)-[:KNOWS]-()<-[:HAS_CREATOR]-(oldPost:Post)-[:HAS_TAG]->(`tag`)
      WHERE oldPost.Post.creationDate < "2011-03-10"
      WITH person, postsOnTag, `tag`, count(oldPost) AS cp
      WHERE cp == 0
      RETURN
        `tag`.`Tag`.name AS tagName,
        sum(postsOnTag) AS postCount
      ORDER BY postCount DESC, tagName ASC
      """
    Then the result should be, in any order:
      | tagName | postCount |

  Scenario: 5. New groups
    When executing query:
      """
      MATCH (person:Person)-[:KNOWS*1..2]-(friend:Person)<-[membership:HAS_MEMBER]-(forum:Forum)
      WHERE id(person) == "person-4139" 
        AND membership.joinDate>"2010-03-10"
        AND not(person==friend)
      WITH DISTINCT friend, forum
      OPTIONAL MATCH (friend)<-[:HAS_CREATOR]-(post:Post)<-[:CONTAINER_OF]-(forum)
      WITH forum, count(post) AS postCount
      RETURN
        id(forum) AS forumId,
        forum.Forum.title AS forumTitle,
        postCount
      ORDER BY postCount DESC, forumId ASC
      LIMIT 20
      """
    Then the result should be, in any order:
      | forumId | forumTitle | postCount |

  @skip
  Scenario: 6. Tag co-occurrence
    # TODO: WHERE patterns
    When executing query:
      """
      MATCH
        (person:Person)-[:KNOWS*1..2]-(friend:Person),
        (friend)<-[:HAS_CREATOR]-(friendPost:Post)-[:HAS_TAG]->(knownTag:`Tag` {name:"$tagName"})
      WHERE id(person) == "" AND not(person==friend)
      MATCH (friendPost)-[:HAS_TAG]->(commonTag:`Tag`)
      WHERE not(commonTag==knownTag)
      WITH DISTINCT commonTag, knownTag, friend
      MATCH (commonTag)<-[:HAS_TAG]-(commonPost:Post)-[:HAS_TAG]->(knownTag)
      WHERE (commonPost)-[:HAS_CREATOR]->(friend)
      RETURN
        commonTag.name AS tagName,
        count(commonPost) AS postCount
      ORDER BY postCount DESC, tagName ASC
      LIMIT 10
      """
    Then a SyntaxError should be raised at runtime:

  @skip
  Scenario: 7. Recent likers
    # TODO: RETURN patterns
    When executing query:
      """
      MATCH (person:Person)<-[:HAS_CREATOR]-(message:Message)<-[like:LIKES]-(liker:Person)
      WHERE id(person) == ""
      WITH liker, message, like.creationDate AS likeTime, person
      ORDER BY likeTime DESC, toInteger(message.id) ASC
      WITH
        liker,
        head(collect({msg: message, likeTime: likeTime})) AS latestLike,
        person
      RETURN
        toInteger(liker.id) AS personId,
        liker.firstName AS personFirstName,
        liker.lastName AS personLastName,
        latestLike.likeTime AS likeCreationDate,
        latestLike.msg.id AS messageId,
        CASE exists(latestLike.msg.content)
          WHEN true THEN latestLike.msg.content
          ELSE latestLike.msg.imageFile
        END AS messageContent,
        latestLike.msg.creationDate AS messageCreationDate,
        not((liker)-[:KNOWS]-(person)) AS isNew
      ORDER BY likeCreationDate DESC, personId ASC
      LIMIT 20
      """
    Then a SyntaxError should be raised at runtime:

  Scenario: 8. Recent replies
    When executing query:
      """
      MATCH (start:Person)<-[:HAS_CREATOR]-(m)<-[:REPLY_OF]-(comment:`Comment`)-[:HAS_CREATOR]->(person:Person)
      WHERE id(start) == "person-4139"
      RETURN
        id(person) AS personId,
        person.Person.firstName AS personFirstName,
        person.Person.lastName AS personLastName,
        comment.`Comment`.creationDate AS commentCreationDate,
        id(comment) AS commentId,
        comment.`Comment`.content AS commentContent
      ORDER BY commentCreationDate DESC, commentId ASC
      LIMIT 20
      """
    Then the result should be, in any order:
      | personId | personFirstName | personLastName | commentCreationDate | commentId | commentContent |

  Scenario: 9. Recent messages by friends or friends of friends
    When executing query:
      """
      MATCH (n:Person)-[:KNOWS*1..2]-(friend:Person)<-[:HAS_CREATOR]-(message)
      WHERE id(n) == "person-4139" AND message.Message.creationDate < "2020-01-01"
      RETURN DISTINCT
        id(friend) AS personId,
        friend.Person.firstName AS personFirstName,
        friend.Person.lastName AS personLastName,
        id(message) AS messageId,
        CASE exists(message.Post.content) 
          WHEN true THEN 
            CASE message.Post.content != ""
              WHEN true THEN message.Post.content 
              ELSE message.Post.imageFile 
            END 
          ELSE message.`Comment`.content 
        END AS messageContent, 
        CASE exists(message.Post.creationDate) 
          WHEN true THEN message.Post.creationDate 
          ELSE message.`Comment`.creationDate
        END AS messageCreationDate
      ORDER BY messageCreationDate DESC, messageId ASC
      LIMIT 20
      """
    Then the result should be, in any order:
      | personId | personFirstName | personLastName | messageId | messageContent | messageCreationDate |

  @skip
  Scenario: 10. Friend recommendation
    # TODO: WHERE patterns, WITH patterns
    When executing query:
      """
      MATCH (person:Person)-[:KNOWS*2..2]-(friend:Person)-[:IS_LOCATED_IN]->(city:Place)
      WHERE id(person) == "" AND
        ((friend.birthday/100%100 = "$month" AND friend.birthday%100 >= 21) OR
        (friend.birthday/100%100 = "$nextMonth" AND friend.birthday%100 < 22))
        AND not(friend=person)
        AND not((friend)-[:KNOWS]-(person))
      WITH DISTINCT friend, city, person
      OPTIONAL MATCH (friend)<-[:HAS_CREATOR]-(post:Post)
      WITH friend, city, collect(post) AS posts, person
      WITH
        friend,
        city,
        length(posts) AS postCount,
        length([p IN posts WHERE (p)-[:HAS_TAG]->(:`Tag`)<-[:HAS_INTEREST]-(person)]) AS commonPostCount
      RETURN
        friend.id AS personId,
        friend.firstName AS personFirstName,
        friend.lastName AS personLastName,
        commonPostCount - (postCount - commonPostCount) AS commonInterestScore,
        friend.gender AS personGender,
        city.name AS personCityName
      ORDER BY commonInterestScore DESC, toInteger(personId) ASC
      LIMIT 10
      """
    Then a SyntaxError should be raised at runtime:

  Scenario: 11. Job referral
    When executing query:
      """
      MATCH (person:Person)-[:KNOWS*1..2]-(friend:Person)
      WHERE id(person) == "person-4139" AND person<>friend
      WITH DISTINCT friend
      MATCH (friend)-[workAt:WORK_AT]->(company:Organisation)-[:IS_LOCATED_IN]->(:Place {name:"Jordan"})
      WHERE workAt.workFrom < "2020"
      RETURN
        id(friend) AS personId,
        friend.Person.firstName AS personFirstName,
        friend.Person.lastName AS personLastName,
        company.Organisation.name AS organizationName,
        workAt.workFrom AS organizationWorkFromYear
      ORDER BY organizationWorkFromYear ASC, personId ASC, organizationName DESC
      LIMIT 10
      """
    Then the result should be, in any order:
      | personId | personFirstName | personLastName | organizationName | organizationWorkFromYear |

  Scenario: 12. Expert search
    # TODO: [:IS_SUBCLASS_OF*0..]
    When executing query:
      """
      MATCH (n:Person)-[:KNOWS]-(friend:Person)<-[:HAS_CREATOR]-(`comment`:`Comment`)-[:REPLY_OF]->(:Post)-[:HAS_TAG]->(`tag`:`Tag`),
        (`tag`)-[:HAS_TYPE]->(tagClass:TagClass)-[:IS_SUBCLASS_OF*0..100]->(baseTagClass:TagClass)
      WHERE id(n)=="" AND (tagClass.TagClass.name == "$tagClassName" OR baseTagClass.TagClass.name == "$tagClassName")
      RETURN
        toInteger(friend.Person.id) AS personId,
        friend.Person.firstName AS personFirstName,
        friend.Person.lastName AS personLastName,
        collect(DISTINCT `tag`.`Tag`.name) AS tagNames,
        count(DISTINCT `comment`) AS replyCount
      ORDER BY replyCount DESC, personId ASC
      LIMIT 20
      """
    Then the result should be, in any order:
      | personId | personFirstName | personLastName | tagNames | replyCount |

  @skip
  Scenario: 13. Single shortest path
    # TODO: shortestPath
    When executing query:
      """
      MATCH (person1:Person {id:$person1Id}), (person2:Person {id:$person2Id})
      OPTIONAL MATCH path = shortestPath((person1)-[:KNOWS*]-(person2))
      RETURN
      CASE path IS NULL
        WHEN true THEN -1
        ELSE length(path)
      END AS shortestPathLength;
      """
    Then a SyntaxError should be raised at runtime:

  @skip
  Scenario: 14. Trusted connection paths
    # TODO: allShortestPaths
    When executing query:
      """
      MATCH path = allShortestPaths((person1:Person {id:$person1Id})-[:KNOWS*..15]-(person2:Person {id:$person2Id}))
      WITH nodes(path) AS pathNodes
      RETURN
        extract(n IN pathNodes | n.id) AS personIdsInPath,
        reduce(weight=0.0, idx IN range(1,size(pathNodes)-1) | extract(prev IN [pathNodes[idx-1]] | extract(curr IN [pathNodes[idx]] | weight + length((curr)<-[:HAS_CREATOR]-(:Comment)-[:REPLY_OF]->(:Post)-[:HAS_CREATOR]->(prev))*1.0 + length((prev)<-[:HAS_CREATOR]-(:Comment)-[:REPLY_OF]->(:Post)-[:HAS_CREATOR]->(curr))*1.0 + length((prev)-[:HAS_CREATOR]-(:Comment)-[:REPLY_OF]-(:Comment)-[:HAS_CREATOR]-(curr))*0.5) )[0][0]) AS pathWight
      ORDER BY pathWight DESC
      """
    Then a SyntaxError should be raised at runtime:
